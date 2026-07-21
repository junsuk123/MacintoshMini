#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "AACDecoderHelix.h"
#include "config.h"
#include "imu_qmi8658.h"

namespace {

Arduino_HWSPI displayBus(
    cfg::PinTftDc, cfg::PinTftCs, cfg::PinTftSck, cfg::PinTftMosi, cfg::PinTftMiso);
Arduino_ST7789 gfx(
    &displayBus,
    cfg::PinTftRst,
    cfg::TftRotation,
    true,
    cfg::TftWidth,
    cfg::TftHeight,
    cfg::TftOffsetX,
    cfg::TftOffsetY,
    0,
    0);
SPIClass sdSpi(HSPI);

struct Clip {
  char name[cfg::FileNameLimit];
};

struct FrameBuffer {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;
};

struct PlayerStats {
  uint32_t framesRead = 0;
  uint32_t framesDecoded = 0;
  uint32_t framesDrawn = 0;
  uint32_t framesDropped = 0;
  uint32_t frameTooLarge = 0;
  uint32_t audioBytes = 0;
  uint32_t audioSamples = 0;   // PCM samples handed to I2S
  uint32_t audioCallbacks = 0; // decoded AAC frames delivered
  uint32_t i2sWriteErrors = 0; // failed/short i2s_write calls
  int audioSampleRate = 0;
  int audioChannels = 0;
  uint32_t lastDecodeUs = 0;  // most recent JPEG decode time
  uint32_t lastDrawUs = 0;    // most recent full-frame SPI draw time
  uint32_t lastReadUs = 0;    // most recent frame read+parse time
};

Clip playlist[cfg::MaxPlaylistItems];
size_t playlistCount = 0;
volatile bool pauseRequested = false;
volatile bool nextRequested = false;
volatile bool stopRequested = false;
// Edge signal from imuVolumeTask: an up/down shake was recognized and playClip
// should toggle pause. Routed through playClip (not by flipping pauseRequested
// here) so the same start-timestamp adjustment as the pause button runs, keeping
// audio and video in sync on resume.
volatile bool shakePauseToggle = false;
PlayerStats stats;

QueueHandle_t freeFrameQueue = nullptr;
QueueHandle_t decodeQueue = nullptr;
QueueHandle_t freeRgbQueue = nullptr;
QueueHandle_t drawRgbQueue = nullptr;
TaskHandle_t decodeTaskHandle = nullptr;
TaskHandle_t drawTaskHandle = nullptr;
SemaphoreHandle_t sdMutex = nullptr;

JPEGDEC jpeg;
FrameBuffer framePool[cfg::DecodeBufferCount];
uint16_t* rgbPool[cfg::RgbFrameCount] = {nullptr};
uint16_t* decodeTarget = nullptr;  // full-frame buffer the decoder is currently filling

i2s_port_t i2sPort = I2S_NUM_0;
int currentAudioRate = 0;
TaskHandle_t audioTaskHandle = nullptr;
volatile bool audioFinished = true;
File audioFile;
libhelix::AACDecoderHelix* aacDecoder = nullptr;

// Playback gain in Q8 fixed point (256 = unity). Written by imuVolumeTask, read
// in the audio callback on every decoded frame.
volatile int16_t audioGainQ8 = 128;

// Volume OSD state shared with the draw task. imuVolumeTask refreshes the linger
// deadline while volume control is active (tilted out of the off zone); the draw
// task overlays a bar until it expires. volumePercent is the level it shows.
volatile int volumePercent = 70;
volatile uint32_t volumeOverlayUntilMs = 0;

// Maps a 0..1 volume setting to a Q8 linear gain. Perceived loudness is roughly
// square-law, so the curve gives finer control at low levels.
inline int16_t volumeSettingToQ8(float setting) {
  if (setting < 0.0f) setting = 0.0f;
  if (setting > 1.0f) setting = 1.0f;
  float g = setting * setting;
  return static_cast<int16_t>(g * 256.0f + 0.5f);
}

void* allocateLarge(size_t bytes) {
  void* ptr = ps_malloc(bytes);
  if (!ptr) {
    ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return ptr;
}

void logMemory(const char* tag) {
  Serial.printf(
      "[mem] %s heap=%u psram=%u frames=%lu/%lu/%lu drop=%lu us(read/dec/draw)=%lu/%lu/%lu audio=%lu\n",
      tag,
      heap_caps_get_free_size(MALLOC_CAP_8BIT),
      ESP.getFreePsram(),
      stats.framesRead,
      stats.framesDecoded,
      stats.framesDrawn,
      stats.framesDropped,
      stats.lastReadUs,
      stats.lastDecodeUs,
      stats.lastDrawUs,
      stats.audioBytes);
}

bool buttonPressed(int pin, uint32_t& lastMs) {
  if (pin < 0) {
    return false;
  }
  if (digitalRead(pin) == LOW) {
    uint32_t now = millis();
    if (now - lastMs > cfg::ButtonDebounceMs) {
      lastMs = now;
      while (digitalRead(pin) == LOW) {
        delay(4);
      }
      return true;
    }
  }
  return false;
}

void drawStatus(const char* line1, const char* line2 = "") {
  gfx.fillScreen(BLACK);
  gfx.setTextColor(WHITE);
  gfx.setTextSize(2);
  gfx.setCursor(10, 86);
  gfx.println(line1);
  gfx.setTextSize(1);
  gfx.setCursor(10, 122);
  gfx.println(line2);
}

esp_err_t initI2s(uint32_t sampleRate) {
  i2s_config_t i2sConfig {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = sampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 6;
  i2sConfig.dma_buf_len = 192;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 0;
  i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
  i2sConfig.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  i2s_pin_config_t pinConfig {};
  pinConfig.mck_io_num = -1;
  pinConfig.bck_io_num = cfg::PinI2sBclk;
  pinConfig.ws_io_num = cfg::PinI2sLrck;
  pinConfig.data_out_num = cfg::PinI2sData;
  pinConfig.data_in_num = -1;

  esp_err_t err = i2s_driver_install(i2sPort, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    return err;
  }
  err = i2s_set_pin(i2sPort, &pinConfig);
  i2s_zero_dma_buffer(i2sPort);
  return err;
}

void audioDataCallback(AACFrameInfo& info, int16_t* pcm, size_t samples, void*) {
  if (currentAudioRate != info.sampRateOut) {
    Serial.printf(
        "[audio] format: %d Hz, %d ch, %d bit\n",
        info.sampRateOut, info.nChans, info.bitsPerSample);
    i2s_set_clk(
        i2sPort,
        info.sampRateOut,
        static_cast<i2s_bits_per_sample_t>(info.bitsPerSample),
        info.nChans == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
    currentAudioRate = info.sampRateOut;
    stats.audioSampleRate = info.sampRateOut;
    stats.audioChannels = info.nChans;
  }

  while (pauseRequested && !stopRequested) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  int16_t gain = audioGainQ8;
  if (gain != 256) {
    for (size_t i = 0; i < samples; ++i) {
      pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * gain) >> 8);
    }
  }

  size_t written = 0;
  esp_err_t err = i2s_write(i2sPort, pcm, samples * sizeof(int16_t), &written, portMAX_DELAY);
  stats.audioCallbacks++;
  stats.audioSamples += samples;
  if (err != ESP_OK || written != samples * sizeof(int16_t)) {
    stats.i2sWriteErrors++;
  }
}

void audioTask(void*) {
  audioFinished = false;
  static uint8_t frame[2048];
  aacDecoder->begin();

  while (!stopRequested && audioFile.available()) {
    if (pauseRequested) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (sdMutex) {
      xSemaphoreTake(sdMutex, portMAX_DELAY);
    }
    int bytesRead = audioFile.read(frame, sizeof(frame));
    if (sdMutex) {
      xSemaphoreGive(sdMutex);
    }
    if (bytesRead <= 0) {
      break;
    }
    stats.audioBytes += bytesRead;

    int remaining = bytesRead;
    uint8_t* cursor = frame;
    while (remaining > 0 && !stopRequested) {
      int consumed = aacDecoder->write(cursor, remaining);
      if (consumed <= 0) {
        break;
      }
      cursor += consumed;
      remaining -= consumed;
    }
  }

  aacDecoder->end();
  i2s_zero_dma_buffer(i2sPort);
  Serial.printf(
      "[audio] done: bytes=%lu callbacks=%lu samples=%lu rate=%d ch=%d i2sErr=%lu reason=%s\n",
      stats.audioBytes,
      stats.audioCallbacks,
      stats.audioSamples,
      stats.audioSampleRate,
      stats.audioChannels,
      stats.i2sWriteErrors,
      stopRequested ? "stop" : "eof");
  audioFinished = true;
  vTaskDelete(nullptr);
}

// Buzzer haptic: the magnetic buzzer on GPIO42 is driven through LEDC PWM so we
// can pick an arbitrary (low) frequency. A short low-frequency burst reads as a
// tactile/audible "tick" acknowledging each volume step.
void buzzerHapticInit() {
  ledcSetup(cfg::BuzzerLedcChannel, cfg::BuzzerHapticFreqDownHz, 10);
  ledcAttachPin(cfg::PinBuzzer, cfg::BuzzerLedcChannel);
  ledcWrite(cfg::BuzzerLedcChannel, 0);  // silent until a tick fires
}

// Emits one tick at freqHz whose loudness follows `loudness` (0..1, the current
// volume). ledcWriteTone sets the pitch at a fixed 50% duty (10-bit scale), then
// we override the duty so the tick gets louder as the volume rises and softer as
// it falls -- the click tracks the level it is acknowledging.
void buzzerTick(uint32_t freqHz, float loudness) {
  if (!cfg::BuzzerHapticEnable) {
    return;
  }
  if (loudness < 0.0f) loudness = 0.0f;
  if (loudness > 1.0f) loudness = 1.0f;
  uint32_t duty = cfg::BuzzerHapticMinDuty +
                  static_cast<uint32_t>(
                      (cfg::BuzzerHapticMaxDuty - cfg::BuzzerHapticMinDuty) * loudness + 0.5f);
  ledcWriteTone(cfg::BuzzerLedcChannel, freqHz);          // set pitch (leaves 50% duty)
  ledcWrite(cfg::BuzzerLedcChannel, duty);                // scale loudness to volume
  vTaskDelay(pdMS_TO_TICKS(cfg::BuzzerHapticMs));
  ledcWrite(cfg::BuzzerLedcChannel, 0);                   // stop
}

// Tilt-to-volume control driven by the onboard QMI8658 IMU.
//
// The screen-plane gravity direction gives a "clock-face" angle. At boot we prime
// a low-pass filter and capture that angle as the zero reference, so volume tracks
// the tilt relative to however the device happens to be sitting. Rotating clockwise
// past the dead-zone raises volume one step every 0.5 s; counter-clockwise lowers it.
// If the IMU is absent the task exits and playback runs at the initial volume.
void imuVolumeTask(void*) {
  if (!imu::begin()) {
    audioGainQ8 = volumeSettingToQ8(cfg::VolumeInitial);
    Serial.println("[imu] volume control disabled; using fixed initial volume");
    vTaskDelete(nullptr);
    return;
  }
  buzzerHapticInit();

  const float alpha = cfg::ImuFilterAlpha;
  float fax = 0, fay = 0, faz = 0;  // EMA low-pass filtered accel axes
  bool primed = false;

  // Warm up the filter (~300 ms) so the baseline isn't taken from a noisy first sample.
  for (int i = 0; i < 15; ++i) {
    int16_t ax, ay, az;
    if (imu::readAccel(ax, ay, az)) {
      if (!primed) {
        fax = ax;
        fay = ay;
        faz = az;
        primed = true;
      } else {
        fax += alpha * (ax - fax);
        fay += alpha * (ay - fay);
        faz += alpha * (az - faz);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::ImuPollIntervalMs));
  }

  const float angle0 = atan2f(fax, fay) * 180.0f / PI;  // baseline = 0 reference
  float volumeSetting = cfg::VolumeInitial;
  audioGainQ8 = volumeSettingToQ8(volumeSetting);
  volumePercent = static_cast<int>(volumeSetting * 100.0f + 0.5f);
  uint32_t lastStepMs = millis();
  Serial.printf("[imu] baseline=%.1f deg, initial volume=%.2f\n", angle0, volumeSetting);

  // Shake-to-pause detector state (see cfg::Shake* for tuning).
  int shakeSign = 0;               // sign of the last counted crossing (+1/-1)
  int shakeCrossings = 0;          // alternating crossings in the current window
  uint32_t shakeWindowStart = 0;   // millis() of the first crossing in the window
  uint32_t shakeCooldownUntil = 0; // ignore shakes / mute volume until this time

  for (;;) {
    int16_t ax, ay, az;
    if (imu::readAccel(ax, ay, az)) {
      fax += alpha * (ax - fax);
      fay += alpha * (ay - fay);
      faz += alpha * (az - faz);

      uint32_t now = millis();

      // --- Up/down shake -> toggle pause ---
      // (fax,fay,faz) is the slow gravity estimate (~1 g); projecting the fast
      // (raw - filtered) residual onto it gives the vertical acceleration, so
      // this responds to up/down motion in any orientation and ignores sideways
      // or in/out shakes. Alternating threshold crossings that complete within
      // ShakeWindowMs are one deliberate shake and raise shakePauseToggle. While
      // a gesture is building or during the post-trigger cooldown we treat the
      // IMU as "shaking" and hold volume steady so the jolt can't nudge volume.
      bool shakeActive = now < shakeCooldownUntil;
      if (cfg::ShakePauseEnable && !shakeActive) {
        float g3 = sqrtf(fax * fax + fay * fay + faz * faz);
        if (g3 > 1.0f) {
          float vert =
              ((ax - fax) * fax + (ay - fay) * fay + (az - faz) * faz) / g3;
          if (shakeCrossings > 0 && now - shakeWindowStart > cfg::ShakeWindowMs) {
            shakeCrossings = 0;  // window lapsed before the gesture completed
            shakeSign = 0;
          }
          int sign = vert > cfg::ShakeAccelThreshold    ? 1
                     : vert < -cfg::ShakeAccelThreshold ? -1
                                                        : 0;
          if (sign != 0 && sign != shakeSign) {
            if (shakeCrossings == 0) shakeWindowStart = now;
            shakeSign = sign;
            if (++shakeCrossings >= cfg::ShakeCrossingsNeeded) {
              shakePauseToggle = true;
              shakeCooldownUntil = now + cfg::ShakeCooldownMs;
              shakeCrossings = 0;
              shakeSign = 0;
              Serial.println("[imu] shake detected -> toggle pause");
            }
          }
          shakeActive = shakeCrossings > 0;
        }
      }

      float delta = atan2f(fax, fay) * 180.0f / PI - angle0;
      while (delta > 180.0f) delta -= 360.0f;   // wrap to -180..180
      while (delta < -180.0f) delta += 360.0f;
      if (cfg::ImuVolumeInvert) delta = -delta;

      if (!shakeActive && fabsf(delta) > cfg::VolumeDeadzoneDeg) {
        // Outside the off zone -> control ON: show the bar and step. The more the
        // device is tilted past the dead zone, the shorter the step interval, so
        // volume ramps faster the harder you tilt.
        volumeOverlayUntilMs = now + cfg::VolumeOverlayLingerMs;
        float speed = (fabsf(delta) - cfg::VolumeDeadzoneDeg) / cfg::VolumeFullSpeedDeg;
        if (speed < 0.0f) speed = 0.0f;
        if (speed > 1.0f) speed = 1.0f;
        uint32_t stepInterval =
            cfg::VolumeStepIntervalMs -
            static_cast<uint32_t>(
                (cfg::VolumeStepIntervalMs - cfg::VolumeStepIntervalMinMs) * speed + 0.5f);
        if (now - lastStepMs >= stepInterval) {
          lastStepMs = now;
          float prev = volumeSetting;
          if (delta > 0.0f) {  // clockwise -> louder
            volumeSetting = min(1.0f, volumeSetting + cfg::VolumeStep);
          } else {  // counter-clockwise -> quieter
            volumeSetting = max(0.0f, volumeSetting - cfg::VolumeStep);
          }
          if (volumeSetting != prev) {  // skip haptic/log when already at a limit
            audioGainQ8 = volumeSettingToQ8(volumeSetting);
            volumePercent = static_cast<int>(volumeSetting * 100.0f + 0.5f);
            buzzerTick(delta > 0.0f ? cfg::BuzzerHapticFreqUpHz : cfg::BuzzerHapticFreqDownHz,
                       volumeSetting);
            Serial.printf("[imu] vol %d%% (delta=%.0f)\n", volumePercent, delta);
          }
        }
      } else {
        // Inside the off zone: hold the timer so re-entry waits a full interval.
        lastStepMs = now;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::ImuPollIntervalMs));
  }
}

bool startAudio(const char* clipName) {
  String path = "/" + String(clipName) + ".aac";
  audioFile = SD.open(path, FILE_READ);
  if (!audioFile || audioFile.isDirectory()) {
    Serial.printf("[audio] no file: %s\n", path.c_str());
    return false;
  }
  Serial.printf("[audio] open %s size=%u bytes\n", path.c_str(), (unsigned)audioFile.size());

  stopRequested = false;
  audioFinished = false;
  BaseType_t ok = xTaskCreatePinnedToCore(
      audioTask, "aac", 4096, nullptr, configMAX_PRIORITIES - 2, &audioTaskHandle, cfg::AudioCore);
  if (ok != pdPASS) {
    Serial.println("[audio] failed to start task");
    audioFile.close();
    audioFinished = true;
    return false;
  }
  return true;
}

void stopAudio() {
  stopRequested = true;
  uint32_t deadline = millis() + 1200;
  while (!audioFinished && millis() < deadline) {
    delay(10);
  }
  if (audioTaskHandle && !audioFinished) {
    vTaskDelete(audioTaskHandle);
    audioFinished = true;
  }
  audioTaskHandle = nullptr;
  if (audioFile) {
    audioFile.close();
  }
}

// Copies one decoded MCU block into its place in the current full-frame buffer.
int drawCallback(JPEGDRAW* pDraw) {
  if (!decodeTarget) {
    return 0;
  }
  const int frameW = cfg::VideoFrameWidth;
  int x = pDraw->x;
  int y = pDraw->y;
  int w = pDraw->iWidth;
  int h = pDraw->iHeight;

  // Clip to the frame in case a block extends past the edges.
  if (x >= frameW || y >= cfg::VideoFrameHeight) {
    return 1;
  }
  if (x + w > frameW) {
    w = frameW - x;
  }
  if (y + h > cfg::VideoFrameHeight) {
    h = cfg::VideoFrameHeight - y;
  }

  const uint16_t* src = pDraw->pPixels;
  for (int row = 0; row < h; ++row) {
    uint16_t* dst = decodeTarget + (y + row) * frameW + x;
    memcpy(dst, src + row * pDraw->iWidth, w * sizeof(uint16_t));
  }
  return 1;
}

void decodeTask(void*) {
  FrameBuffer* frame = nullptr;
  while (xQueueReceive(decodeQueue, &frame, portMAX_DELAY) == pdTRUE) {
    if (!frame) {
      break;
    }

    uint16_t* fb = nullptr;
    if (xQueueReceive(freeRgbQueue, &fb, portMAX_DELAY) != pdTRUE || !fb) {
      xQueueSend(freeFrameQueue, &frame, portMAX_DELAY);
      continue;
    }
    decodeTarget = fb;

    bool ok = false;
    uint32_t t0 = micros();
    if (jpeg.openRAM(frame->data, frame->size, drawCallback)) {
      jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
      jpeg.setMaxOutputSize(cfg::MaxMcuPixels / 256);
      ok = jpeg.decode(0, 0, 0) != 0;
      jpeg.close();
    }
    stats.lastDecodeUs = micros() - t0;
    decodeTarget = nullptr;

    if (ok) {
      stats.framesDecoded++;
      xQueueSend(drawRgbQueue, &fb, portMAX_DELAY);
    } else {
      stats.framesDropped++;
      xQueueSend(freeRgbQueue, &fb, portMAX_DELAY);
    }

    xQueueSend(freeFrameQueue, &frame, portMAX_DELAY);
  }
  vTaskDelete(nullptr);
}

// Volume OSD overlaid on the bottom-center of the video area. The next video
// frame fully redraws this region, so the bar self-erases once it stops being
// drawn -- no manual clearing needed. Only the draw task touches gfx during
// playback, so this is race-free with the frame push above it.
void drawVolumeOverlay(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  const int barW = 180;
  const int barH = 14;
  const int x = (cfg::VideoFrameWidth - barW) / 2;
  const int y = 208;
  const int fillW = (barW - 4) * percent / 100;

  gfx.setTextColor(WHITE, BLACK);
  gfx.setTextSize(2);
  gfx.setCursor(x, y - 26);
  gfx.printf("VOL %3d%%", percent);

  gfx.fillRect(x, y, barW, barH, DARKGREY);              // track
  if (fillW > 0) {
    gfx.fillRect(x + 2, y + 2, fillW, barH - 4, GREEN);  // filled level
  }
  gfx.drawRect(x, y, barW, barH, WHITE);                 // border
}

void drawTask(void*) {
  uint16_t* fb = nullptr;
  uint16_t* lastFb = nullptr;    // most recent frame, retained so the volume
                                 // overlay can be refreshed over it while paused
  bool pauseOverlayShown = false;
  for (;;) {
    if (xQueueReceive(drawRgbQueue, &fb, pdMS_TO_TICKS(80)) == pdTRUE) {
      if (!fb) {
        break;  // shutdown sentinel
      }
      // One large contiguous transfer per frame instead of dozens of small ones.
      uint32_t t0 = micros();
      gfx.draw16bitRGBBitmap(
          0, cfg::VideoTopMargin, fb, cfg::VideoFrameWidth, cfg::VideoFrameHeight);
      if (millis() < volumeOverlayUntilMs) {
        drawVolumeOverlay(volumePercent);
      }
      stats.lastDrawUs = micros() - t0;
      stats.framesDrawn++;
      pauseOverlayShown = false;
      // Keep this frame around for pause-time refreshes; recycle the previous one.
      if (lastFb) {
        xQueueSend(freeRgbQueue, &lastFb, portMAX_DELAY);
      }
      lastFb = fb;
    } else if (pauseRequested && lastFb) {
      // Paused: no new frames arrive, so refresh the retained frame here to keep
      // the volume overlay live while the user adjusts volume. Redrawing the
      // frame first erases the previous bar, so it self-clears once the overlay
      // lingers out -- same contract as during playback. Do nothing when there is
      // nothing to show, leaving the frozen frame untouched.
      bool overlayActive = millis() < volumeOverlayUntilMs;
      if (overlayActive || pauseOverlayShown) {
        gfx.draw16bitRGBBitmap(
            0, cfg::VideoTopMargin, lastFb, cfg::VideoFrameWidth, cfg::VideoFrameHeight);
        if (overlayActive) {
          drawVolumeOverlay(volumePercent);
          pauseOverlayShown = true;
        } else {
          pauseOverlayShown = false;  // erased on this pass; stop refreshing
        }
      }
    }
  }
  if (lastFb) {
    xQueueSend(freeRgbQueue, &lastFb, portMAX_DELAY);
  }
  vTaskDelete(nullptr);
}

bool initVideoPipeline() {
  freeFrameQueue = xQueueCreate(cfg::DecodeBufferCount, sizeof(FrameBuffer*));
  decodeQueue = xQueueCreate(cfg::DecodeBufferCount, sizeof(FrameBuffer*));
  freeRgbQueue = xQueueCreate(cfg::RgbFrameCount, sizeof(uint16_t*));
  drawRgbQueue = xQueueCreate(cfg::RgbFrameCount, sizeof(uint16_t*));
  if (!freeFrameQueue || !decodeQueue || !freeRgbQueue || !drawRgbQueue) {
    return false;
  }

  for (size_t i = 0; i < cfg::DecodeBufferCount; ++i) {
    framePool[i].capacity = cfg::JpegFrameBufferSize;
    framePool[i].data = static_cast<uint8_t*>(allocateLarge(framePool[i].capacity));
    if (!framePool[i].data) {
      return false;
    }
    FrameBuffer* ptr = &framePool[i];
    xQueueSend(freeFrameQueue, &ptr, 0);
  }

  const size_t frameBytes =
      static_cast<size_t>(cfg::VideoFrameWidth) * cfg::VideoFrameHeight * sizeof(uint16_t);
  for (size_t i = 0; i < cfg::RgbFrameCount; ++i) {
    rgbPool[i] = static_cast<uint16_t*>(allocateLarge(frameBytes));
    if (!rgbPool[i]) {
      return false;
    }
    xQueueSend(freeRgbQueue, &rgbPool[i], 0);
  }

  BaseType_t ok1 = xTaskCreatePinnedToCore(
      decodeTask, "jpeg", 4096, nullptr, configMAX_PRIORITIES - 3, &decodeTaskHandle, cfg::DecodeCore);
  BaseType_t ok2 = xTaskCreatePinnedToCore(
      drawTask, "draw", 4096, nullptr, configMAX_PRIORITIES - 4, &drawTaskHandle, cfg::DrawCore);
  return ok1 == pdPASS && ok2 == pdPASS;
}

void shutdownVideoPipeline() {
  FrameBuffer* endFrame = nullptr;
  uint16_t* endFb = nullptr;
  if (decodeQueue) {
    xQueueSend(decodeQueue, &endFrame, pdMS_TO_TICKS(50));
  }
  if (drawRgbQueue) {
    xQueueSend(drawRgbQueue, &endFb, pdMS_TO_TICKS(50));
  }
}

bool readNextJpegFrame(
    File& file,
    FrameBuffer* frame,
    uint8_t* readBuf,
    size_t& readPos,
    size_t& buffered) {
  frame->size = 0;
  bool started = false;
  uint8_t prev = 0;

  // No per-byte file.available() here: it enters the FatFs/VFS layer on every
  // byte and dominated the read cost. We refill in bulk and stop when read()
  // returns nothing.
  while (true) {
    if (readPos >= buffered) {
      if (sdMutex) {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
      }
      int bytes = file.read(readBuf, cfg::ReadBufferSize);
      if (sdMutex) {
        xSemaphoreGive(sdMutex);
      }
      if (bytes <= 0) {
        return false;
      }
      readPos = 0;
      buffered = bytes;
    }

    uint8_t b = readBuf[readPos++];

    if (!started) {
      if (prev == 0xFF && b == 0xD8) {
        started = true;
        if (frame->capacity < 2) {
          return false;
        }
        frame->data[0] = 0xFF;
        frame->data[1] = 0xD8;
        frame->size = 2;
      }
      prev = b;
      continue;
    }

    if (frame->size >= frame->capacity) {
      stats.frameTooLarge++;
      return false;
    }
    frame->data[frame->size++] = b;

    if (prev == 0xFF && b == 0xD9) {
      stats.framesRead++;
      return true;
    }
    prev = b;
  }
  return false;
}

void scanPlaylist(File dir, const String& prefix = "/") {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    String name = entry.name();
    if (entry.isDirectory()) {
      scanPlaylist(entry, prefix + name + "/");
      entry.close();
      continue;
    }

    if (name.endsWith(".mjpeg") && playlistCount < cfg::MaxPlaylistItems) {
      int slash = name.lastIndexOf('/');
      String stem = slash >= 0 ? name.substring(slash + 1) : name;
      stem.remove(stem.length() - 6);
      // Reserved intro/transition stems never join the auto-scanned playlist.
      if (stem != cfg::LoadingClipName && stem != cfg::TransitionClipName) {
        stem.toCharArray(playlist[playlistCount].name, cfg::FileNameLimit);
        playlistCount++;
      }
    }
    entry.close();
  }
}

bool playClip(const char* clipName) {
  String videoPath = "/" + String(clipName) + ".mjpeg";
  File videoFile = SD.open(videoPath, FILE_READ);
  if (!videoFile || videoFile.isDirectory()) {
    Serial.printf("[video] missing: %s\n", videoPath.c_str());
    return false;
  }

  Serial.printf("[play] %s\n", clipName);
  drawStatus("Playing", clipName);
  startAudio(clipName);

  uint8_t* readBuf = static_cast<uint8_t*>(heap_caps_malloc(cfg::ReadBufferSize, MALLOC_CAP_8BIT));
  if (!readBuf) {
    videoFile.close();
    stopAudio();
    return false;
  }
  size_t buffered = 0;
  size_t readPos = 0;

  uint32_t lastPause = 0;
  uint32_t lastNext = 0;
  uint32_t lastTelemetry = 0;
  uint32_t pauseStarted = 0;
  uint32_t frameIndex = 0;
  uint32_t start = millis();
  pauseRequested = false;
  nextRequested = false;
  stopRequested = false;
  shakePauseToggle = false;

  while (videoFile.available() && !nextRequested) {
    // Pause toggles from either the physical button or an up/down shake take the
    // same path: flip pauseRequested and, on resume, advance `start` by the paused
    // duration. The audio task holds its exact byte position while paused and the
    // video pacing clock is shifted to match, so both resume from the same instant
    // in sync. The shake check runs even while paused, so a second shake resumes.
    bool togglePause = buttonPressed(cfg::PinButtonPause, lastPause);
    if (shakePauseToggle) {
      shakePauseToggle = false;
      togglePause = true;
    }
    if (togglePause) {
      pauseRequested = !pauseRequested;
      if (pauseRequested) {
        pauseStarted = millis();
      } else if (pauseStarted != 0) {
        start += millis() - pauseStarted;
        pauseStarted = 0;
      }
      Serial.printf("[ui] pause=%d\n", pauseRequested);
    }
    if (buttonPressed(cfg::PinButtonNext, lastNext)) {
      nextRequested = true;
      break;
    }
    if (pauseRequested) {
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }

    FrameBuffer* frame = nullptr;
    if (xQueueReceive(freeFrameQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE || !frame) {
      stats.framesDropped++;
      continue;
    }

    uint32_t tRead = micros();
    bool gotFrame = readNextJpegFrame(videoFile, frame, readBuf, readPos, buffered);
    stats.lastReadUs = micros() - tRead;
    if (!gotFrame) {
      xQueueSend(freeFrameQueue, &frame, portMAX_DELAY);
      break;
    }

    xQueueSend(decodeQueue, &frame, portMAX_DELAY);
    frameIndex++;

    uint32_t targetMs = start + (frameIndex * 1000UL / cfg::VideoFps);
    while (millis() < targetMs && !pauseRequested && !nextRequested) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (millis() - lastTelemetry > cfg::TelemetryIntervalMs) {
      lastTelemetry = millis();
      logMemory("play");
    }
  }

  stopRequested = true;
  heap_caps_free(readBuf);
  videoFile.close();
  stopAudio();
  vTaskDelay(pdMS_TO_TICKS(80));
  logMemory("clip-end");
  return true;
}

void initButtons() {
  if (cfg::PinButtonPause >= 0) {
    pinMode(cfg::PinButtonPause, INPUT_PULLUP);
  }
  if (cfg::PinButtonNext >= 0) {
    pinMode(cfg::PinButtonNext, INPUT_PULLUP);
  }
  pinMode(cfg::PinBuzzer, OUTPUT);
  digitalWrite(cfg::PinBuzzer, LOW);
  if (cfg::UseBoardV2PowerPins) {
    pinMode(cfg::PinPowerHold, OUTPUT);
    digitalWrite(cfg::PinPowerHold, HIGH);
    pinMode(cfg::PinPowerButton, INPUT);
  }
}

bool initDisplay() {
  if (!gfx.begin(cfg::TftFrequency)) {
    return false;
  }
  pinMode(cfg::PinTftBl, OUTPUT);
  digitalWrite(cfg::PinTftBl, HIGH);
  drawStatus("Macintosh Mini", "booting");
  return true;
}

// Sends a raw SPI command frame and returns the R1 response byte.
// 0x01 = idle (card present & talking), 0xFF = no response (bus/card dead).
uint8_t sdSpiCommand(uint8_t cmd, uint32_t arg, uint8_t crc) {
  sdSpi.transfer(0x40 | cmd);
  sdSpi.transfer((arg >> 24) & 0xFF);
  sdSpi.transfer((arg >> 16) & 0xFF);
  sdSpi.transfer((arg >> 8) & 0xFF);
  sdSpi.transfer(arg & 0xFF);
  sdSpi.transfer(crc);
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 8; ++i) {
    r1 = sdSpi.transfer(0xFF);
    if ((r1 & 0x80) == 0) {
      break;
    }
  }
  return r1;
}

// Low-level electrical probe: is a card physically responding on this wiring?
// Distinguishes wiring/power/no-card faults from filesystem/mount faults.
bool probeSdBus(int cs, int sck, int miso, int mosi) {
  pinMode(miso, INPUT_PULLUP);  // GPIO3 (MISO) is a floating strap pin; needs a pull-up
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);

  sdSpi.end();
  sdSpi.begin(sck, miso, mosi, cs);
  sdSpi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  // >=74 clocks with CS high to wake the card into SPI mode.
  digitalWrite(cs, HIGH);
  for (int i = 0; i < 12; ++i) {
    sdSpi.transfer(0xFF);
  }

  digitalWrite(cs, LOW);
  uint8_t r1cmd0 = sdSpiCommand(0, 0, 0x95);  // CMD0 GO_IDLE_STATE
  uint8_t r1cmd8 = 0xFF;
  uint8_t voltage[4] = {0, 0, 0, 0};
  if (r1cmd0 == 0x01) {
    r1cmd8 = sdSpiCommand(8, 0x000001AA, 0x87);  // CMD8 SEND_IF_COND
    if (r1cmd8 == 0x01) {
      for (int i = 0; i < 4; ++i) {
        voltage[i] = sdSpi.transfer(0xFF);
      }
    }
  }
  digitalWrite(cs, HIGH);
  sdSpi.transfer(0xFF);
  sdSpi.endTransaction();

  Serial.printf(
      "[sd-probe] cs=%d sck=%d miso=%d mosi=%d @400kHz -> CMD0 R1=0x%02X",
      cs, sck, miso, mosi, r1cmd0);
  if (r1cmd0 == 0x01) {
    Serial.printf(" (idle OK), CMD8 R1=0x%02X echo=%02X%02X%02X%02X",
                  r1cmd8, voltage[0], voltage[1], voltage[2], voltage[3]);
  } else if (r1cmd0 == 0xFF) {
    Serial.print(" -> NO RESPONSE (check wiring/power/card inserted)");
  } else {
    Serial.print(" -> unexpected (partial comms; check MISO pull-up / signal integrity)");
  }
  Serial.println();
  return r1cmd0 == 0x01;
}

// Attempts to mount at escalating frequencies and reports the precise stage that fails.
bool mountSdAt(int cs, int sck, int miso, int mosi) {
  // Highest first so playback runs at full bandwidth; step down for marginal cards.
  const uint32_t frequencies[] = {cfg::SdFrequency, 20000000, 4000000, 1000000, 400000};
  sdSpi.end();
  pinMode(miso, INPUT_PULLUP);
  sdSpi.begin(sck, miso, mosi, cs);

  for (uint32_t frequency : frequencies) {
    Serial.printf("[sd] SD.begin cs=%d sck=%d miso=%d mosi=%d freq=%lu ... ",
                  cs, sck, miso, mosi, frequency);
    if (SD.begin(cs, sdSpi, frequency)) {
      uint8_t type = SD.cardType();
      const char* typeName = type == CARD_NONE ? "NONE"
                             : type == CARD_MMC ? "MMC"
                             : type == CARD_SD  ? "SDSC"
                             : type == CARD_SDHC ? "SDHC/SDXC"
                                                 : "UNKNOWN";
      if (type == CARD_NONE) {
        Serial.println("mounted but CARD_NONE (no usable card)");
        SD.end();
        delay(50);
        continue;
      }
      Serial.printf("MOUNTED type=%s size=%lluMB\n",
                    typeName, SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }
    Serial.println("FAILED");
    SD.end();
    delay(50);
  }
  return false;
}

bool initSd() {
  const int cs = cfg::PinSdCs;
  const int sck = cfg::PinSdSck;
  const int miso = cfg::PinSdMiso;
  const int mosi = cfg::PinSdMosi;

  Serial.println("[sd] === SD diagnostics (documented wiring) ===");
  bool electrical = probeSdBus(cs, sck, miso, mosi);
  if (!electrical) {
    Serial.println("[sd] electrical probe failed -> card not responding at SPI layer");
  } else {
    Serial.println("[sd] electrical probe OK -> card responds; proceeding to mount");
  }

  if (mountSdAt(cs, sck, miso, mosi)) {
    return true;
  }

  if (electrical) {
    Serial.println(
        "[sd] card responds to CMD0 but SD.begin failed at every frequency -> "
        "likely filesystem/format issue (use FAT32) or marginal signal at speed");
    return false;
  }

  // No electrical response on the documented wiring: fall back to a pin sweep
  // in case the harness is miswired, logging the reason for each attempt.
  Serial.println("[sd] falling back to pin permutation sweep...");
  const int candidatePins[] = {cfg::PinSdCs, cfg::PinSdSck, cfg::PinSdMiso, cfg::PinSdMosi};
  for (int c : candidatePins) {
    for (int s : candidatePins) {
      if (s == c) continue;
      for (int mi : candidatePins) {
        if (mi == c || mi == s) continue;
        for (int mo : candidatePins) {
          if (mo == c || mo == s || mo == mi) continue;
          if (c == cs && s == sck && mi == miso && mo == mosi) continue;  // already tried
          if (probeSdBus(c, s, mi, mo) && mountSdAt(c, s, mi, mo)) {
            Serial.printf("[sd] WORKING WIRING FOUND: cs=%d sck=%d miso=%d mosi=%d\n",
                          c, s, mi, mo);
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Plays a reserved screen only if its .mjpeg is present. A missing file is the
// documented "disable this screen" path, so it is skipped silently (info log,
// never an error) and the caller continues normally.
void playSpecialClip(const char* clipName) {
  String path = "/" + String(clipName) + ".mjpeg";
  if (SD.exists(path)) {
    playClip(clipName);
  } else {
    Serial.printf("[special] %s absent, skipping\n", path.c_str());
  }
}

void playLoadingScreen() {
  playSpecialClip(cfg::LoadingClipName);
}

}  // namespace

void setup() {
  WiFi.mode(WIFI_OFF);
  Serial.begin(cfg::SerialBaud);
  delay(300);
  Serial.println("\nMacintosh Mini firmware");

  sdMutex = xSemaphoreCreateMutex();
  initButtons();
  if (!initDisplay()) {
    Serial.println("[fatal] display init failed");
  }
  if (initI2s(cfg::DefaultAudioRate) != ESP_OK) {
    Serial.println("[fatal] i2s init failed");
  }

  static libhelix::AACDecoderHelix decoder(audioDataCallback);
  aacDecoder = &decoder;

  // Tilt-to-volume runs on the audio core at low priority; it mostly sleeps.
  xTaskCreatePinnedToCore(
      imuVolumeTask, "imuVol", 4096, nullptr, 1, nullptr, cfg::AudioCore);

  if (!initVideoPipeline()) {
    drawStatus("Video init failed");
    Serial.println("[fatal] video pipeline init failed");
    return;
  }

  if (!initSd()) {
    drawStatus("SD card failed");
    Serial.println("[fatal] sd init failed");
    return;
  }

  File root = SD.open("/");
  scanPlaylist(root);
  root.close();

  Serial.printf("[playlist] %u clips\n", static_cast<unsigned>(playlistCount));
  for (size_t i = 0; i < playlistCount; ++i) {
    Serial.printf("  %u: %s\n", static_cast<unsigned>(i + 1), playlist[i].name);
  }
  logMemory("ready");

  playLoadingScreen();
}

void loop() {
  if (playlistCount == 0) {
    drawStatus("No videos", "copy *.mjpeg and *.aac to SD");
    delay(1000);
    return;
  }

  // A transition screen (if present) plays between consecutive videos. The static
  // flag persists across loop() iterations, so the boundary on the loop wrap
  // (last -> first) also gets a transition, but the very first video after the
  // loading screen does not.
  static bool firstVideo = true;
  for (size_t i = 0; i < playlistCount; ++i) {
    if (!firstVideo) {
      playSpecialClip(cfg::TransitionClipName);
    }
    firstVideo = false;
    playClip(playlist[i].name);
  }

  if (!cfg::LoopPlaylist) {
    drawStatus("Done");
    shutdownVideoPipeline();
    if (cfg::DeepSleepWhenDone) {
      digitalWrite(cfg::PinTftBl, LOW);
      gfx.displayOff();
      esp_deep_sleep_start();
    }
    while (true) {
      delay(1000);
    }
  }
}
