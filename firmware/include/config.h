#pragma once

#include <Arduino.h>

namespace cfg {

constexpr uint32_t SerialBaud = 115200;
constexpr uint8_t VideoFps = 25;
constexpr bool LoopPlaylist = true;
constexpr bool DeepSleepWhenDone = false;

constexpr int PinButtonPause = -1;
constexpr int PinButtonNext = -1;

constexpr int PinPowerHold = 41;
constexpr int PinPowerButton = 40;
constexpr int PinBuzzer = 42;
constexpr bool UseBoardV2PowerPins = true;

constexpr int PinSdCs = 3;
constexpr int PinSdMosi = 16;
constexpr int PinSdSck = 17;
constexpr int PinSdMiso = 18;
constexpr uint32_t SdFrequency = 40000000;

constexpr int PinTftBl = 15;
constexpr int PinTftDc = 4;
constexpr int PinTftCs = 5;
constexpr int PinTftRst = 8;
constexpr int PinTftMosi = 7;
constexpr int PinTftSck = 6;
constexpr int PinTftMiso = -1;
constexpr int TftRotation = 0;
constexpr int TftWidth = 240;
constexpr int TftHeight = 280;
constexpr int TftOffsetX = 0;
constexpr int TftOffsetY = 20;
constexpr uint32_t TftFrequency = 80000000;

constexpr int PinI2sBclk = 2;
constexpr int PinI2sLrck = 44;
constexpr int PinI2sData = 43;
constexpr uint32_t DefaultAudioRate = 44100;

// --- IMU (QMI8658) tilt-based volume control ---
// I2C bus shared with the onboard QMI8658 6-axis IMU (addr 0x6B). GPIO10/11 are
// unused by the video/audio/SD paths. Pins follow the Waveshare reference design;
// verify against the board schematic. If WHO_AM_I fails at boot the volume task
// disables itself and playback continues normally.
constexpr int PinI2cSda = 11;
constexpr int PinI2cScl = 10;
constexpr uint8_t ImuI2cAddress = 0x6B;

constexpr uint32_t ImuPollIntervalMs = 20;      // 50 Hz accel sampling
constexpr float ImuFilterAlpha = 0.15f;         // EMA low-pass on accel (lower = smoother/slower)
constexpr float VolumeDeadzoneDeg = 15.0f;      // off zone: tilt within this of baseline = control OFF
constexpr uint32_t VolumeStepIntervalMs = 500;  // one volume step per 0.5 s while tilted
constexpr float VolumeStep = 0.05f;             // 0..1 setting change per step (20 steps full range)
constexpr float VolumeInitial = 0.70f;          // startup volume setting (pre perceptual curve)
constexpr bool ImuVolumeInvert = false;         // flip if clockwise lowers instead of raises volume
constexpr uint32_t VolumeOverlayLingerMs = 1200; // keep the on-screen bar visible this long after activity

// --- Buzzer haptic feedback on volume steps (magnetic buzzer on GPIO42) ---
// Driven far below the buzzer's ~2-4 kHz resonance to favor a low tactile "tick"
// over an audible beep. A coil buzzer has little moving mass, so expect a faint
// click you mostly hear -- not vibration-motor-grade haptics. Tune freq/duration
// on-device. Up ticks slightly higher than down so direction is distinguishable.
constexpr bool BuzzerHapticEnable = true;
constexpr int BuzzerLedcChannel = 4;             // LEDC channel dedicated to the buzzer
constexpr uint32_t BuzzerHapticFreqUpHz = 70;    // volume-up tick pitch
constexpr uint32_t BuzzerHapticFreqDownHz = 45;  // volume-down tick pitch (lower = deeper)
constexpr uint32_t BuzzerHapticMs = 25;          // tick duration

// --- Shake-to-pause (up/down shake toggles playback pause) ---
// The IMU task projects the dynamic (gravity-removed) acceleration onto the
// gravity vector to isolate vertical motion, then counts alternating threshold
// crossings. A deliberate up/down shake toggles pause via the same path as the
// pause button, so audio and video stop together and resume in sync. Thresholds
// are in raw accel LSB (accel is +-2g full scale, so 1 g ~= 16384 LSB); tune on
// device if shakes trigger too easily or not at all.
constexpr bool ShakePauseEnable = true;
constexpr float ShakeAccelThreshold = 6000.0f;   // vertical dynamic accel to count a crossing (~0.37 g)
constexpr int ShakeCrossingsNeeded = 3;          // alternating up/down crossings that make one gesture
constexpr uint32_t ShakeWindowMs = 700;          // crossings must complete within this span
constexpr uint32_t ShakeCooldownMs = 1200;       // debounce after a toggle; also mutes tilt-volume during a shake

constexpr BaseType_t AudioCore = 0;
constexpr BaseType_t DecodeCore = 0;
constexpr BaseType_t DrawCore = 1;

constexpr size_t FileNameLimit = 48;
constexpr size_t MaxPlaylistItems = 96;

// --- Optional intro / transition screens (independent of the playlist) ---
// Reserved clip stems, excluded from the auto-scanned playlist. Put
// "<name>.mjpeg" (and optionally "<name>.aac") on the SD card to enable a screen;
// delete the file to disable it and the system skips it silently, no error.
//   Loading    -> plays once at boot, before the playlist
//   Transition -> plays between videos (including the loop wrap), not before the
//                 first video after loading
constexpr char LoadingClipName[] = "Loading";
constexpr char TransitionClipName[] = "Transition";
constexpr size_t ReadBufferSize = 8192;
constexpr size_t JpegFrameBufferSize = 240 * 280 * 2 / 5;
constexpr size_t DecodeBufferCount = 3;
constexpr size_t MaxMcuPixels = 16 * 16 * 8;

// Full-frame RGB565 buffers in PSRAM: whole frame decoded once, then pushed to
// the panel in a single large transfer instead of many small per-MCU writes.
constexpr int VideoFrameWidth = 240;
constexpr int VideoFrameHeight = 240;
constexpr int VideoTopMargin = 0;      // panel offset already applied by the driver
constexpr size_t RgbFrameCount = 3;    // triple buffering to decouple decode/draw jitter

constexpr uint32_t ButtonDebounceMs = 180;
constexpr uint32_t TelemetryIntervalMs = 2000;

}  // namespace cfg
