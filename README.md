# Macintosh Mini

Waveshare ESP32-S3-LCD-1.69 보드를 기반으로 만든 미니 매킨토시 형태의 SD 카드 비디오 플레이어입니다. 펌웨어는 MicroSD의 MJPEG 비디오와 AAC 오디오를 읽어 내장 ST7789V2 LCD와 외장 MAX98357 I2S 앰프로 재생합니다.

![Macintosh Mini](docs/assets/macintosh-mini-hero.png)

## 현재 구조

최종 사용 코드는 `firmware/`의 PlatformIO 프로젝트로 통합되어 있습니다. 예전 Arduino IDE 스케치와 패키징된 변환기 실행 파일은 유지 대상이 아닙니다.

```text
firmware/
  platformio.ini
  partitions.csv
  include/
    config.h
    imu_qmi8658.h
  src/
    main.cpp
  tools/
    convert_video.py
    validate_sd_media.py
docs/
  assets/
    macintosh-mini-hero.png
    physical-wiring-diagram.svg
    esp32s3169.png
    microSDReader.png
    max98357A.png
```

## 하드웨어

기준 보드는 Waveshare `ESP32-S3-LCD-1.69` V2입니다.

- ESP32-S3, 16 MB Flash, 8 MB PSRAM
- 1.69 inch 240x280 ST7789V2 LCD
- QMI8658 IMU
- PCF85063 RTC
- LiPo 충전 회로, 전원 버튼, 부저
- 외장 MicroSD SPI 모듈
- 외장 MAX98357 I2S 앰프

![Physical wiring diagram](docs/assets/physical-wiring-diagram.svg)

## 핀 구성

| 기능 | 신호 | GPIO |
| --- | --- | --- |
| LCD | SCK | 6 |
| LCD | MOSI/SDA | 7 |
| LCD | CS | 5 |
| LCD | DC | 4 |
| LCD | RST | 8 |
| LCD | BL | 15 |
| MicroSD | CS | 3 |
| MicroSD | MOSI | 16 |
| MicroSD | SCK | 17 |
| MicroSD | MISO | 18 |
| MAX98357 | BCLK | 2 |
| MAX98357 | LRC/WS | 44 |
| MAX98357 | DIN | 43 |
| QMI8658 | SDA | 11 |
| QMI8658 | SCL | 10 |
| V2 power hold | SYS_EN | 41 |
| V2 power input | SYS_OUT | 40 |
| V2 buzzer | BUZZ | 42 |

핀과 동작 값은 [firmware/include/config.h](firmware/include/config.h)에 모여 있습니다. 구버전 보드나 배선 변경이 있으면 이 파일만 수정하면 됩니다.

## 소프트웨어 특성

- PlatformIO + Arduino framework 기반 ESP32-S3 펌웨어
- `Arduino_GFX`로 ST7789V2 LCD 구동
- `JPEGDEC`로 MJPEG 프레임 디코딩
- `arduino-libhelix` AAC 디코더와 I2S DMA 오디오 출력
- FreeRTOS 태스크로 AAC 디코딩, JPEG 디코딩, LCD 전송 분리
- PSRAM 기반 JPEG/RGB565 버퍼 풀과 큐로 프레임 소유권 관리
- QMI8658 IMU 기울기 기반 볼륨 조절 및 부저 피드백
- SD 카드 전기적 응답과 마운트 실패 단계를 시리얼 로그로 진단
- `Loading` 및 `Transition` 예약 클립 지원

## SD 카드 파일 규칙

SD 카드 루트에 같은 이름의 `.mjpeg`와 `.aac` 파일을 둡니다.

```text
/Loading.mjpeg
/Loading.aac
/Transition.mjpeg
/Transition.aac
/sample-01.mjpeg
/sample-01.aac
```

`Loading`은 부팅 후 한 번 재생됩니다. `Transition`은 영상 사이에 재생됩니다. 둘 다 선택 사항이며, 해당 `.mjpeg` 파일이 없으면 자동으로 건너뜁니다. 일반 재생 목록은 SD 카드에서 검색한 `*.mjpeg` 파일로 구성되고, 같은 이름의 `.aac` 파일이 있으면 오디오도 함께 재생됩니다.

## 영상 변환

PC에 `ffmpeg`가 설치되어 있어야 합니다.

```bash
cd firmware
python tools/convert_video.py input.mp4 --out sdcard --name sample-01 --fps 25
```

권장 값:

- 해상도: 240x240
- FPS: 25
- 비디오: MJPEG, `q:v 8-12`
- 오디오: AAC ADTS, mono, 44.1 kHz, 48-64 kbps

변환한 파일을 검증하려면 다음 명령을 실행합니다.

```bash
cd firmware
python tools/validate_sd_media.py sdcard --name sample-01 --config include/config.h
```

## 빌드와 업로드

```bash
cd firmware
pio run
pio run -t upload
pio device monitor
```

VS Code에서는 PlatformIO 확장을 설치하고 `firmware/` 폴더를 열면 됩니다. USB-JTAG 디버깅은 `platformio.ini`의 `debug_tool = esp-builtin` 설정을 사용합니다.

## 참고 링크

- Waveshare ESP32-S3-LCD-1.69: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.69
- Arduino_GFX: https://github.com/moononournation/Arduino_GFX
- JPEGDEC: https://github.com/bitbank2/JPEGDEC
- arduino-libhelix: https://github.com/pschatzmann/arduino-libhelix
