/***
 * 필요한 라이브러리
 * Arduino_GFX: https://github.com/moononournation/Arduino_GFX.git
 * libhelix: https://github.com/pschatzmann/arduino-libhelix.git
 * JPEGDEC: https://github.com/bitbank2/JPEGDEC.git
 */
unsigned long pause_start_ms = 0;

#include <WiFi.h>
#include <FS.h>
#include <SPI.h>
#include <FFat.h>
#include <LittleFS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPIFFS.h>
#include <Arduino_GFX_Library.h>
/* 오디오 태스크 */
#include "esp32_audio_task.h"

/* 비디오 테스크*/
#include "mjpeg_decode_draw_task.h"
//종속 파일


int buttonPin = 17; // 버튼 핀 (마우스 핀)
int clicked=0;


char LOAD_FILENAME[50]="A"; // 대상 문자열 정의
char CLEAN_FILENAME[50]="B";
struct FileName{
  char FILENAME[50]; // 대상 문자열 정의
};
FileName files[100];
int Ind=0;


#define FPS 25
#define MJPEG_BUFFER_SIZE (240 * 240 * 2 / 8)

#define AUDIOASSIGNCORE 1
#define DECODEASSIGNCORE 0
#define DRAWASSIGNCORE 1

#define SS 10       // SD카드 CS핀
#define SD_MOSI 11  //  SPI SD_MOSI
#define SD_SCK 12   //SPI SD_SCK
#define SD_MISO 13  //SPI SD_MISO


#define TFT_BRIGHTNESS 250  // 디스플레이 밝기 설정
#define GFX_BL 33           // 디스플레이 백라이트 핀
#define TFT_DC 37           // 디스플레이 DC 핀
#define TFT_CS -1           // 디스플레이 CS 핀 (사용하지 않음)
#define TFT_RES 38          // 디스플레이 Reset 핀
#define TFT_ROT 4           // 화면 회전
#define TFT_MOSI 34         //  SPI TFT_MOSI
#define TFT_SCK 21          //SPI TFT_SCK
#define TFT_MISO -1         //SPI TFT_MISO
#define TFT_WIDTH 240       // 디스플레이 가로 픽셀
#define TFT_HEIGHT 240      // 디스플레이 세로 픽셀

//
Arduino_HWSPI *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);  // DC/CS/SD_SCK/SD_MOSI/SD_MISO -> SPI 통신 핀 설정

Arduino_ST7789 *gfx = new Arduino_ST7789(bus, TFT_RES, TFT_ROT, true, TFT_WIDTH, TFT_HEIGHT, 0, 0);  // 디스플레이 설정

/* variables */
static int next_frame = 0;
static int skipped_frames = 0;
static unsigned long start_ms, curr_ms, next_frame_ms;

static int drawMCU(JPEGDRAW *pDraw)
{
  unsigned long s = millis();
  gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video_ms += millis() - s;
  return 1;
} /* drawMCU() */

void setup()
{ 
  WiFi.mode(WIFI_OFF);

  Serial.begin(115200);
  Serial.println("MJPEG_2task_Audio_1task");
  pinMode(buttonPin,INPUT);
  
#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  Serial.println("Init display");
  if (!gfx->begin(80000000))
  {
    Serial.println("Init display failed!");
  }
  gfx->fillScreen(BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  Serial.println("Init I2S");
  gfx->println("Init I2S");
#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 3 /* SCLK */, 1 /* LRCK */, 5 /* DOUT */, -1 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32S2)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 3 /* SCLK */, 1 /* LRCK */, 5 /* DOUT */, -1 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32S3)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 3 /* SCLK */, 1 /* LRCK */, 5 /* DOUT */, -1 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32C3)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 3 /* SCLK */, 1 /* LRCK */, 5 /* DOUT */, -1 /* DIN */);
#endif
  if (ret_val != ESP_OK) {
    Serial.printf("i2s_init failed: %d\n", ret_val);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("Init FS");
  gfx->println("Init FS");

  SPIClass spi = SPIClass(HSPI);
  spi.begin(SD_SCK,SD_MISO,SD_MOSI,SS);
  gfx->println("Define SPICLASS Success!!");
  gfx->println("SPI begin success!!");
  if (!SD.begin(SS,spi,80000000))
  {
    Serial.println("ERROR: File system mount failed!");
    gfx->println("ERROR: SD Card!!");
  }
  else
  { 
    File root = SD.open("/");
    searchDirectory(root, 0);
    Serial.println("searching done!");//파일 탐색
    root.close();
    playVideo(LOAD_FILENAME);
    for(int i=0; i<Ind; i++){
      playVideo(CLEAN_FILENAME);
      playVideo(files[i].FILENAME);
    }
  }

#ifdef GFX_BL
  digitalWrite(GFX_BL, LOW);
#endif
  gfx->displayOff();
  // esp_deep_sleep_start();
}

void loop() {
}

void searchDirectory(File dir, int numTabs) {//SD카드에서 비디오 파일을 찾은 후 파일명을 구조체 배열에 저장
  while (true) {
    File entry =  dir.openNextFile();
        String temp=entry.name();

    if (! entry) {
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      
    }
     if (!entry.isDirectory() && temp.endsWith(".mjpeg")&&strcmp("load.mjpeg",temp.c_str())&&strcmp("clean.mjpeg",temp.c_str())) {
      Serial.println(entry.name());
      gfx->println(entry.name());
      temp.remove(temp.length()-6,6);
      strcpy(files[Ind].FILENAME,temp.c_str());
      Ind++;
    } else if (entry.isDirectory()) {
      if(strcmp(entry.name(),"doing object")!=0) searchDirectory(entry, numTabs + 1);
    }
    entry.close();
  }
}

void playVideo(char file[50]) {
  bool aac_file_available = false;
  Serial.println("Open AAC file");
  gfx->println("Open AAC file");
  File aFile = SD.open("/" + String(file) + ".aac");
  Serial.println(aFile.name());

  if (aFile) {
    aac_file_available = true;
  } else {
    Serial.println("Failed to open aac file");
    //aFile = SD.open("/"+String(a_files));
  }

  if (!aFile || aFile.isDirectory()) {
    Serial.println("ERROR: Failed to open");
    gfx->println("ERROR: Failed to open");
  } 
    File vFile = SD.open("/" + String(file) + ".mjpeg");
    Serial.println(vFile.name());
    _inputindex = 0;
    _buf_read = 0;
    _mjpeg_buf_offset = 0;
    _mBufIdx = 0;
    if (!vFile || vFile.isDirectory()) {
      Serial.println("failed to open mjpeg file!!");
    } else {
      gfx->println("Init video");
      mjpeg_setup(&vFile, MJPEG_BUFFER_SIZE, drawMCU,
                  false /* useBigEndian */, DECODEASSIGNCORE, DRAWASSIGNCORE);
      gfx->println("Start play audio task");
      BaseType_t ret_val;
      if(aac_file_available){
        if (aac_file_available) {
          ret_val = aac_player_task_start(&aFile, AUDIOASSIGNCORE);//오디오 재생 시작
        } else {
          Serial.println("No audio file");
        }
        if (ret_val != pdPASS) {
          Serial.printf("Audio player task start failed: %d\n", ret_val);
          gfx->printf("Audio player task start failed: %d\n", ret_val);
        }
      }
      Serial.println("Start play video");
      gfx->println("Start play video");
      start_ms = millis();
      curr_ms = millis();
      next_frame_ms = start_ms + (++next_frame * 1000 / FPS / 2);
      // 영상 재생 중인지를 나타내는 변수
      bool isPlaying = true;

      while (vFile.available() && mjpeg_read_frame()) {
        // 현재 버튼 상태를 읽어옴
        bool isClicked = (digitalRead(buttonPin) == HIGH);
        if(isClicked&&isPlaying){
          isPlaying = false;//재생 상태 업데이트
          Serial.println("Pause mjpeg!!");
          pause_start_ms = millis();// 일시 정지 시간을 기록
        }
        else if(isClicked&&!isPlaying){
          isPlaying = true;//재생 상태 업데이트
          Serial.println("Resume mjpeg");
          long pause_duration = millis() - pause_start_ms;//일시정지 시작 시각을 현재시간에서 차감(일시정지 시간 기록)
          start_ms += pause_duration;//동영상 재생 시작 시각에 일시 정지 시간 추가
          next_frame_ms += pause_duration;
        }

        if (isPlaying) {
          // 영상을 재생 중일 때만 처리
          total_read_video_ms += millis() - curr_ms;
          curr_ms = millis();
          if (millis() < next_frame_ms) {
            // 영상 재생
            mjpeg_draw_frame();
            total_decode_video_ms += millis() - curr_ms;
            curr_ms = millis();
          } else {
            ++skipped_frames;
            Serial.println("Skip frame");
          }

          while (millis() < next_frame_ms) {
            vTaskDelay(pdMS_TO_TICKS(1));
          }

          curr_ms = millis();
          next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
        }
      }

      int time_used = millis() - start_ms;
      int total_frames = next_frame - 1;
      Serial.println("AV end");
      vFile.close();
      if(aac_file_available){
        aFile.close();
      }

      next_frame = 0;
      skipped_frames = 0;
      delay(200);
      mjpeg_cleanup();
      delay(200);
    }
  
}