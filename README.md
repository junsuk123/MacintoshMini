MacintoshMini
아주 작은 사이즈의 미니어처 매킨토시를 위한 비디오 디코딩 프로젝트

이 프로젝트는 MJPEG 비디오 디코딩과 오디오 출력을 지원하는 미니 매킨토시 모형 제작을 목표로 합니다. LOLIN S3 Mini V1.0.0 ESP32-S3FH4R2 와이파이 블루투스 개발보드를 메인 컨트롤러로 사용하며, 1.3인치 TFT LCD와 I2S 오디오 앰프 등을 사용해 작은 디스플레이와 오디오 출력을 구현합니다.

참고 자료 깃허브: RGB565 비디오 디코딩
사용된 부품
LOLIN S3 Mini V1.0.0 ESP32-S3FH4R2 와이파이 블루투스 개발보드
1.3인치 TFT LCD (ER-TFTM013-1, 240x240 픽셀, IPS, ST7789 드라이버)
SD 카드 소켓 모듈
MAX98357 I2S 3W 오디오 앰프
14x20mm 1W 미니스피커 [FQ-031]
3.7V 220mAh LiPo 배터리
회로 연결
SCK (TFT) --------------------> GPIO 21
MOSI (TFT) -------------------> GPIO 34
MISO (SD) -------------------> GPIO 13
DC_TFT -----------------------> GPIO 37
RES_TFT ----------------------> GPIO 38
CS_SD ------------------------> GPIO 10
BL_TFT (백라이트) -------------> GPIO 33
추가 연결 (오디오 I2S)
BCK (Bit Clock) -----------------> GPIO 26
LRCK (Left/Right Clock) --------> GPIO 25
DATA (I2S Data) -----------------> GPIO 22
메모리 및 멀티태스킹 설정
이 프로젝트는 ESP32의 듀얼 코어 기능을 활용하여 비디오 디코딩 및 출력, 오디오 재생 등을 동시에 처리합니다. 메모리 할당에 영향을 줄 수 있는 주요 변수는 다음과 같습니다:

MJPEG_BUFFER_SIZE: 240 * 240 * 2 / 4 (MJPEG 버퍼 크기 설정)
xTaskCreatePinnedToCore: 파일 디코딩 작업을 core 0에서 실행합니다.
xTaskCreatePinnedToCore(displaying, "Task1", 57600, NULL, 1, &Task1, 0);
MAXOUTPUTSIZE: MJPEG 구성의 각 JPEG 프레임 크기 설정 (16)
NUMBER_OF_DRAW_BUFFER: 확인할 프레임 버퍼 수 설정
xTaskCreatePinnedToCore(drawTask): 디스플레이 출력 작업을 core 0에서 실행합니다.
xTaskCreatePinnedToCore(drawTask, "drawTask", 16000, &_p, 1, &task, 1);
이벤트 실행 코어: 기타 이벤트는 core 1에서 처리합니다.
Arduino 메인 코드: Arduino 메인 루프는 core 0에서 실행됩니다.
설치 방법
ESP32 설정:

Arduino IDE에서 ESP32 보드를 설치합니다.
ESP32 설치 가이드를 참고하여 환경을 설정합니다.
프로젝트 파일:

MiniMacinPro3.ino 및 관련 헤더 파일들을 프로젝트 폴더에 복사합니다.
TFT LCD, I2S 오디오 앰프, SD 카드 모듈, 스피커 등을 ESP32 보드에 적절히 연결합니다.
라이브러리 설치:

프로젝트에서 사용되는 라이브러리를 설치해야 합니다. 아래 라이브러리들을 Arduino 라이브러리 매니저에서 설치합니다:
Adafruit GFX Library
Adafruit ST7735 and ST7789 Library
ESP32 I2S Library
업로드:

모든 연결을 완료한 후, Arduino IDE에서 MiniMacinPro3.ino 파일을 선택하고 보드에 업로드합니다.
주요 기능
MJPEG 비디오 디코딩:

MJPEG 파일을 디코딩하여 LCD 화면에 표시합니다.
FreeRTOS를 활용한 비동기 작업 처리로, 비디오 프레임을 빠르게 처리합니다.
AAC 오디오 디코딩:

MAX98357 I2S 앰프를 사용하여 오디오를 재생합니다.
AAC 파일을 실시간으로 디코딩하고 스피커로 출력합니다.
파일 저장 및 불러오기:

마이크로 SD 카드 모듈을 통해 비디오 및 오디오 파일을 저장하거나 불러올 수 있습니다.
참고 자료
아두이노 공식 웹사이트: Arduino
ESP32 개발 자료: ESP32 Documentation
TFT LCD 및 I2S 앰프 데이터시트: 제공된 자료 링크 참고
