#define VIDEO_WIDTH 240//비디오 파일 규격 설정
#define VIDEO_HEIGHT 240
#define RGB565_FILENAME "/video.rgb"
#define RGB565_BUFFER_SIZE (VIDEO_WIDTH * VIDEO_HEIGHT * 2)//가로*세로*2바이트

#include <WiFi.h>
#include <SD.h>
#include <SD_MMC.h>
#include <Arduino_GFX_Library.h>
#define TFT_BRIGHTNESS 128
#define SCK 7
#define MOSI 9
#define MISO 8
#define SS 5
#define TFT_BL 3

// ST7789 Display
Arduino_HWSPI *bus = new Arduino_HWSPI(2,-1, SCK, MOSI, MISO);//DC/CD/XCK/MOSI/MISO->SPI 통신 핀 세팅
Arduino_ST7789 *gfx = new Arduino_ST7789(bus, 1, 4 , true , 240 , 240, 0 , 0 );//RES/rotation/IPS/WIDTH/HEIGHT/COLOFFSET/ROWOFFSET-> 디스플레이 세팅

void setup() {
  WiFi.mode(WIFI_OFF);//와이파이 설정
  Serial.begin(115200);//시리얼 통신 시작

  gfx->begin();// 디스플레이 동작
  gfx->fillScreen(YELLOW);//전체화면 노란색 출력(동작 테스트용)
  delay(2000);
  gfx->fillScreen(BLACK);//화면 리셋
  delay(2000);

#ifdef TFT_BL
  ledcSetup(1, 12000, 8);    // 12 kHz PWM, 8-bit resolution->LED 백라이트 설정
  ledcAttachPin(TFT_BL, 1);  // assign TFT_BL pin to channel 1
  ledcWrite(1, TFT_BRIGHTNESS);
#endif

  // Init SD card
  if (!SD.begin(SS, SPI, 16000000)) // SPI bus mode-> SD 카드 동작 및 가능 여부 확인
  {
    Serial.println(F("ERROR: SD card mount failed!"));
    gfx->println(F("ERROR: SD card mount failed!"));
  } else {
    File vFile = SD.open(RGB565_FILENAME);//미리 지정한 파일을 파일 변수에 저장
    // File vFile = SD_MMC.open(RGB565_FILENAME);
    if (!vFile || vFile.isDirectory()) {//파일 존재 여부 확인
      Serial.println(F("ERROR: Failed to open " RGB565_FILENAME " file for reading"));
      gfx->println(F("ERROR: Failed to open " RGB565_FILENAME " file for reading"));
    } else {//파일 읽고 디스플레이에 출력
      uint8_t *buf = (uint8_t *)malloc(RGB565_BUFFER_SIZE);//미리 설정한 버퍼 사이즈 만큼 8비트 단위의 버퍼 동적 할당
      if (!buf) {//영상 끝 확인
        Serial.println(F("buf malloc failed!"));
      } else {
        Serial.println(F("RGB565 video start"));
        gfx->setAddrWindow((gfx->width() - VIDEO_WIDTH) / 2, (gfx->height() - VIDEO_HEIGHT) / 2, VIDEO_WIDTH, VIDEO_HEIGHT);

        while (vFile.available()) {
          // Read video
          uint32_t l = vFile.read(buf, RGB565_BUFFER_SIZE);
          // Play video
          gfx->startWrite();
          gfx->writeBytes(buf, l);
          gfx->endWrite();
        }
        Serial.println(F("RGB565 video end"));
        vFile.close();
      }
    }
  }
#ifdef TFT_BL
  delay(6000);
  ledcDetachPin(TFT_BL);

#endif
  gfx->displayOff();
}

void loop() {
}
