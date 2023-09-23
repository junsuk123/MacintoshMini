# MacintoshMini

아주 작은 사이즈의 미니어처 매킨토시를 위한 비디오 디코딩 프로젝트

-참고 자료 깃허브 :https://github.com/moononournation/RGB565_video.git

- 사용한 부품
    1. Seeed Studio XIAO ESP32S3
    2. er-tftm013-1(1.3inch 240*240 pixcel IPS TFT Display with ST7789 driver)
    3. SD카드 소켓 모듈
    4. 3.7v 220mah LiPoqo배터리
- 회로 연결
- ![회로도 ps](https://github.com/junsuk123/MacintoshMini/assets/36057196/84737aef-259a-4c72-b19b-c8113bc85dac)

    - SCK-------------------->GPIO7
    - MOSI------------------->GPIO9
    - MISO------------------->GPIO8
    - DC_TFT----------------->GPIO2
    - RES_TFT---------------->GPIO1
    - CS_SD------------------>GPIO4

 - 메모리 할당 문제에 영향을 줄 수 있는 변수 들
   1. MJPEG_BUFFER_SIZE (240 * 240 * 2 / 4)//MJPEG 버퍼 크기 설정 (픽셀)
   2. xTaskCreatePinnedToCore(// 파일 디코딩을 core0에서 실
      displaying,     // 태스크 함수
      "Task1",        // 태스크 이름
      57600,          // 스택 크기 (워드 단위)
      NULL,           // 태스크 파라미터
      1,              // 태스크 우선순위
      &Task1,         // 태스크 핸들
      0);
   3. MAXOUTPUTSIZE 16//Mjpeg를 구성하는 하나의 jpeg 프레임에 대한 크기
   4. NUMBER_OF_DRAW_BUFFER//얼마나 많은 프레임들을 확인할 건지 지정하는 크기
   5. xTaskCreatePinnedToCore(drawTask, "drawTask", 16000, &_p, 1, &task, 1);//디스플레이 출력을 core 0에서 실행
   6. Events run on core 1// 기타 이벤트 등은 core 1에서 실행
   7. Arduino run on core 0// 아두이노 주 코드들은 core 0에서 실행
  
      

  #MJPEG #DECODE #MEMORY #MULTITASK #DUALCORE #ST7789 #SD
