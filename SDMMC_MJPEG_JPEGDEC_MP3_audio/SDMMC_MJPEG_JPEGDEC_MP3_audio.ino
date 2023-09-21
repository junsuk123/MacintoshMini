//SDMMC_MJPEG_JPEGDEC_MP3_audio.ino

// MP3 오디오 파일과 MJPEG 비디오 파일을 재생하고 화면에 표시하는 Arduino 코드입니다.

// MP3 파일의 경로 및 이름 설정
#define MP3_FILENAME "/video.mp3"
// 초당 이미지 프레임 설정
#define FPS 30
// MJPEG 이미지 파일의 경로 및 이름 설정
#define MJPEG_FILENAME "/video.mjpeg"
// MJPEG 버퍼 크기 설정 (픽셀)
#define MJPEG_BUFFER_SIZE (240 * 240 * 2 / 4)

// 필요한 라이브러리 포함
#include <WiFi.h>          // 인터넷 연결 서비스를 위한 라이브러리 (현재 미사용)
#include <SD_MMC.h>        // SD 카드 관련 라이브러리
#include <SD.h>            // SD 카드 관련 라이브러리
#include <Arduino_GFX_Library.h> // 디스플레이 그래픽 관련 라이브러리

// SPI 통신을 위한 핀 설정
#define SCK 7
#define MOSI 9
#define MISO 8

#define SS 4 // SD 카드 CS 핀

#define TFT_BRIGHTNESS 128 // 디스플레이 밝기 설정
#define TFT_BL 3           // 디스플레이 백라이트 핀
#define TFT_DC 2           // 디스플레이 DC 핀
#define TFT_CS -1          // 디스플레이 CS 핀 (사용하지 않음)
#define TFT_RES 1          // 디스플레이 Reset 핀
#define TFT_ROT 4          // 화면 회전
#define TFT_WIDTH 240      // 디스플레이 가로 픽셀
#define TFT_HEIGHT 240     // 디스플레이 세로 픽셀

// ST7789 디스플레이 설정
Arduino_HWSPI *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, SCK, MOSI, MISO); // DC/CS/SCK/MOSI/MISO -> SPI 통신 핀 설정
Arduino_ST7789 *gfx = new Arduino_ST7789(bus, TFT_RES, TFT_ROT, true, TFT_WIDTH, TFT_HEIGHT, 0, 0); // 디스플레이 설정

/* MP3 오디오 */
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
static AudioGeneratorMP3 *mp3;
static AudioOutputI2S *out;

/* MJPEG 비디오 */
#include "MjpegClass.h"
static MjpegClass mjpeg;
uint8_t *mjpeg_buf;

/* 변수들 */
static unsigned long total_play_audio, total_read_video, total_decode_video, total_show_video;
static unsigned long start_ms, curr_ms, next_frame_ms;
static int skipped_frames, next_frame, time_used, total_frames;
TaskHandle_t Task1;

// 픽셀 그리기 콜백
static int drawMCU(JPEGDRAW *pDraw)
{
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
} /* drawMCU() */

void setup()
{
  WiFi.mode(WIFI_OFF);
  Serial.begin(115200);

  // 비디오 초기화
  gfx->begin();

  gfx->fillScreen(YELLOW);
  delay(2000);
  gfx->fillScreen(BLACK);
  delay(1000);

#ifdef TFT_BL
  ledcSetup(1, 12000, 8);       // 12 kHz PWM, 8-bit resolution
  ledcAttachPin(TFT_BL, 1);     // TFT_BL 핀을 채널 1에 연결
  ledcWrite(1, TFT_BRIGHTNESS); // 밝기 설정 (0 - 255)
#endif

  // SD 카드 초기화
  if (!SD.begin(SS, SPI, 80000000)) // SPI 버스 모드로 SD 카드 초기화 및 가능 여부 확인
  {
    Serial.println(F("ERROR: SD card mount failed!"));
    gfx->println(F("ERROR: SD card mount failed!"));
    exit(1);
  }
  Serial.println(F("SD Success!!"));

  out = new AudioOutputI2S(0, 1, 64); // 내장 DAC로 출력 설정
  mp3 = new AudioGeneratorMP3();

  mjpeg_buf = (uint8_t *)malloc(MJPEG_BUFFER_SIZE);
  xTaskCreatePinnedToCore(
      displaying,     // 태스크 함수
      "Task1",        // 태스크 이름
      57600,          // 스택 크기 (워드 단위)
      NULL,           // 태스크 파라미터
      1,              // 태스크 우선순위
      &Task1,         // 태스크 핸들
      0);

#ifdef TFT_BL
  delay(6000);
  ledcDetachPin(TFT_BL);
#endif
  gfx->displayOff();
}

void displaying(void *param)
{
  Serial.print(F("# Task 1 running on core "));
  Serial.println(F(xPortGetCoreID()));

  if (!mjpeg_buf)
  {
    Serial.println(F("mjpeg_buf malloc failed!"));
    gfx->println(F("mjpeg_buf malloc failed!"));
    exit(1);
  }
  AudioFileSourceFS *aFile = new AudioFileSourceFS(SD, MP3_FILENAME);
  File vFile = SD.open(MJPEG_FILENAME);
  if (!vFile || vFile.isDirectory())
  {
    Serial.println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
    gfx->println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
    exit(1);
  }

  Serial.println(F("PCM audio MJPEG video start"));

  // 비디오 초기화
  mjpeg.setup(&vFile, mjpeg_buf, drawMCU, true, true);

  // 오디오 초기화
  mp3->begin(aFile, out);

  skipped_frames = 0;
  total_play_audio = 0;
  total_read_video = 0;
  total_decode_video = 0;
  total_show_video = 0;
  next_frame = 0;
  total_frames = 0;
  start_ms = millis();
  curr_ms = start_ms;
  next_frame_ms = start_ms + (++next_frame * 1000 / FPS);

  while (vFile.available() && mjpeg.readMjpegBuf()) // 비디오 읽기
  {
    // 비디오 파일에 할당된 최소 스택 크기
    Serial.println(configMINIMAL_STACK_SIZE);

    total_read_video += millis() - curr_ms;
    curr_ms = millis();

    if (millis() < next_frame_ms) // 프레임 표시 또는 스킵 확인
    {
      // 비디오 재생
      mjpeg.drawJpg();
      total_decode_video += millis() - curr_ms;
    }
    else
    {
      ++skipped_frames;
      Serial.println(F("Skip frame"));
    }
    curr_ms = millis();

    // 오디오 재생
    if ((mp3->isRunning()) && (!mp3->loop()))
    {
      mp3->stop();
    }
    total_play_audio += millis() - curr_ms;

    while (millis() < next_frame_ms) //다음 프레임 시간까지 대기
    {
      vTaskDelay(1);
    }

    curr_ms = millis();
    next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
  }
  time_used = millis() - start_ms;
  Serial.println(F("PCM audio MJPEG video end"));
  vFile.close();//비디오 및 오디오 파일 닫기
  aFile->close();

  display_stat();

  delay(10000);
}

void loop()
{
}

#define CHART_MARGIN 24
#define LEGEND_A_COLOR 0xE0C3
#define LEGEND_B_COLOR 0x33F7
#define LEGEND_C_COLOR 0x4D69
#define LEGEND_D_COLOR 0x9A74
#define LEGEND_E_COLOR 0xFBE0
#define LEGEND_F_COLOR 0xFFE6
#define LEGEND_G_COLOR 0xA2A5


// 차트 및 통계 정보 표시 함수
void display_stat()
{
  int played_frames = total_frames - skipped_frames;
  float fps = 1000.0 * played_frames / time_used;
  Serial.printf("Played frames: %d\n", played_frames);
  Serial.printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
  Serial.printf("Time used: %d ms\n", time_used);
  Serial.printf("Expected FPS: %d\n", FPS);
  Serial.printf("Actual FPS: %0.1f\n", fps);
  Serial.printf("Play MP3: %lu ms (%0.1f %%)\n", total_play_audio, 100.0 * total_play_audio / time_used);
  Serial.printf("SDMMC read MJPEG: %lu ms (%0.1f %%)\n", total_read_video, 100.0 * total_read_video / time_used);
  Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video, 100.0 * total_decode_video / time_used);
  Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video, 100.0 * total_show_video / time_used);

  gfx->setCursor(0, 0);
  gfx->setTextColor(WHITE);
  gfx->printf("Played frames: %d\n", played_frames);
  gfx->printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
  gfx->printf("Actual FPS: %0.1f\n\n", fps);
  int16_t r1 = ((gfx->height() - CHART_MARGIN - CHART_MARGIN) / 2);
  int16_t r2 = r1 / 2;
  int16_t cx = gfx->width() - gfx->height() + CHART_MARGIN + CHART_MARGIN - 1 + r1;
  int16_t cy = r1 + CHART_MARGIN;
  float arc_start = 0;
  float arc_end = max(2.0, 360.0 * total_play_audio / time_used);
  for (int i = arc_start + 1; i < arc_end; i += 2)
  {
    gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_D_COLOR);
  }
  gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_D_COLOR);
  gfx->setTextColor(LEGEND_D_COLOR);
  gfx->printf("Play MP3:\n%0.1f %%\n", 100.0 * total_play_audio / time_used);

  arc_start = arc_end;
  arc_end += max(2.0, 360.0 * total_read_video / time_used);
  for (int i = arc_start + 1; i < arc_end; i += 2)
  {
    gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_C_COLOR);
  }
  gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_C_COLOR);
  gfx->setTextColor(LEGEND_C_COLOR);
  gfx->printf("Read MJPEG:\n%0.1f %%\n", 100.0 * total_read_video / time_used);

  arc_start = arc_end;
  arc_end += max(2.0, 360.0 * total_decode_video / time_used);
  for (int i = arc_start + 1; i < arc_end; i += 2)
  {
    gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_B_COLOR);
  }
  gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_B_COLOR);
  gfx->setTextColor(LEGEND_B_COLOR);
  gfx->printf("Decode video:\n%0.1f %%\n", 100.0 * total_decode_video / time_used);

  arc_start = arc_end;
  arc_end += max(2.0, 360.0 * total_show_video / time_used);
  for (int i = arc_start + 1; i < arc_end; i += 2)
  {
    gfx->fillArc(cx, cy, r2, 0, arc_start - 90.0, i - 90.0, LEGEND_A_COLOR);
  }
  gfx->fillArc(cx, cy, r2, 0, arc_start - 90.0, arc_end - 90.0, LEGEND_A_COLOR);
  gfx->setTextColor(LEGEND_A_COLOR);
  gfx->printf("Play video:\n%0.1f %%\n", 100.0 * total_show_video / time_used);
}
