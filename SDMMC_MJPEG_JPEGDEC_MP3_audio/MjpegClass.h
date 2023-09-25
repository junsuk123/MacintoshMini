//MjpegClass.h
#ifndef _MJPEGCLASS_H_
#define _MJPEGCLASS_H_

#define READ_BUFFER_SIZE 1024
#define MAXOUTPUTSIZE 8
#define NUMBER_OF_DRAW_BUFFER 3
#define DRAWTASK_STACKDEPTH  1600
#define DRAWTASK_CORE 0


#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_heap_caps.h>
#include <FS.h>

#include <JPEGDEC.h>

typedef struct
{
  JPEG_DRAW_CALLBACK *drawFunc;
} paramDrawTask;

static xQueueHandle xqh = 0;
static JPEGDRAW jpegdraws[NUMBER_OF_DRAW_BUFFER];
static int queue_cnt, draw_cnt;

static int queueDrawMCU(JPEGDRAW *pDraw)
{
  ++queue_cnt;
  while ((queue_cnt - draw_cnt) > NUMBER_OF_DRAW_BUFFER)
  {
    delay(1);
  }

  int len = pDraw->iWidth * pDraw->iHeight * 2;
  JPEGDRAW *j = &jpegdraws[queue_cnt % NUMBER_OF_DRAW_BUFFER];
  j->x = pDraw->x;
  j->y = pDraw->y;
  j->iWidth = pDraw->iWidth;
  j->iHeight = pDraw->iHeight;
  memcpy(j->pPixels, pDraw->pPixels, len);

  xQueueSend(xqh, &j, 0);
  return 1;
}

static void drawTask(void *arg)
{
  paramDrawTask *p = (paramDrawTask *)arg;
  uint16_t *drawBuffer = NULL;

  for (int i = 0; i < NUMBER_OF_DRAW_BUFFER; i++)
  {
    // 더 이상 사용하지 않는 버퍼는 해제
    if (drawBuffer != NULL)
    {
      free(drawBuffer);
      drawBuffer = NULL;
    }

    // 새로운 drawBuffer 할당
    drawBuffer = (uint16_t *)heap_caps_malloc(MAXOUTPUTSIZE * 16 * 16 * 2, MALLOC_CAP_DMA);
    if (drawBuffer == NULL)
    {
      // 메모리 할당 실패 처리
      Serial.println(F("Failed to allocate memory for drawBuffer"));
      vTaskDelete(NULL);
    }

    // 버퍼 할당이 완료되었으므로 할당된 drawBuffer를 사용
    jpegdraws[i].pPixels = drawBuffer;
  }
  JPEGDRAW *pDraw;
  //Serial.println("drawTask start");
  while (xQueueReceive(xqh, &pDraw, portMAX_DELAY))
  {
    // Serial.printf("task work: x: %d, y: %d, iWidth: %d, iHeight: %d\r\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    p->drawFunc(pDraw);
    // Serial.println("task work done");
    ++draw_cnt;
  }
  if (drawBuffer != NULL)
  {
    free(drawBuffer);
    drawBuffer = NULL;
  }

  vQueueDelete(xqh);
  vTaskDelete(NULL);
}

class MjpegClass
{
public:
  bool setup(Stream *input, uint8_t *mjpeg_buf, JPEG_DRAW_CALLBACK *pfnDraw, bool enableMultiTask, bool useBigEndian)
  {
    _input = input;
    _mjpeg_buf = mjpeg_buf;
    _pfnDraw = pfnDraw;
    _enableMultiTask = enableMultiTask;
    _useBigEndian = useBigEndian;

    _mjpeg_buf_offset = 0;
    _inputindex = 0;
    _remain = 0;

    queue_cnt = 0;
    draw_cnt = 0;

    if (!_read_buf)
    {
      _read_buf = (uint8_t *)malloc(READ_BUFFER_SIZE);
    }

    if (_enableMultiTask)//만약 멀티태스크 허용되면
    {
      if (!xqh)//큐 헨들러가 0일때(초기화 되지 않았을 때)
      {
        TaskHandle_t task;
        _p.drawFunc = pfnDraw;
        xqh = xQueueCreate(NUMBER_OF_DRAW_BUFFER, sizeof(JPEGDRAW));//JPEG 프레임 만큼의 버퍼 큐를 미리 지정한 용량 만큼 설정한다.
        xTaskCreatePinnedToCore(drawTask, "drawTask",DRAWTASK_STACKDEPTH, &_p, 1, &task, DRAWTASK_CORE);//DrawTaskCore에서 drawTask를 실행하며 DrawRaskMalloc만큼 메모리를 할당한다.
      }
    }

    return true;
  }

  bool readMjpegBuf()// Mjpeg 버퍼를 읽음
  {
    if (_inputindex == 0)//만약 첫 데이터라면
    {
      _buf_read = _input->readBytes(_read_buf, READ_BUFFER_SIZE);//입력 버퍼에 지정 할당한 메모리 크기만큼 데이터를 읽어 _buf_read에 저장한다.
      _inputindex += _buf_read;//인덱스 하나 추가
    }
    _mjpeg_buf_offset = 0;
    int i = 0;
    bool found_FFD8 = false;
    while ((_buf_read > 0) && (!found_FFD8))//프레임 헤더를 찾는 반복문
    {
      i = 0;
      while ((i < _buf_read) && (!found_FFD8))//_buf_read 내에서 FFD8(비디오 파일의 헤더 파일)을 찾을 때까지 반복
      {
        if ((_read_buf[i] == 0xFF) && (_read_buf[i + 1] == 0xD8)) // JPEG header-->FFD8
        {
          // Serial.printf("Found FFD8 at: %d.\n", i);
          found_FFD8 = true;
        }
        ++i;
      }
      if (found_FFD8)//만약 헤더 파일을 찾았으면
      {
        --i;//해당 위치의 데이터를 읽기 위해 i를 1 만큼 차감
      }
      else
      {
        _buf_read = _input->readBytes(_read_buf, READ_BUFFER_SIZE);//탐색을 위해 _buf_read 다시 처음으로 되돌리기
      }
    }
    uint8_t *_p = _read_buf + i;//이전에 찾은 위치로 돌아가 해당 위치에서 포인터 생성
    _buf_read -= i;
    bool found_FFD9 = false;
    if (_buf_read > 0)//_buf_read가 첫 데이터가 아니라면
    {
      i = 3;
      while ((_buf_read > 0) && (!found_FFD9))//프레임 트레일러를 찾을 때까지 반복문
      {
        if ((_mjpeg_buf_offset > 0) && (_mjpeg_buf[_mjpeg_buf_offset - 1] == 0xFF) && (_p[0] == 0xD9)) // JPEG trailer--->FFD9
        {
          // Serial.printf("Found FFD9 at: %d.\n", i);
          found_FFD9 = true;
        }
        else
        {
          while ((i < _buf_read) && (!found_FFD9))//_buf_read 범위 내에서 파일의 트레일러 파일을 찾으면
          {
            if ((_p[i] == 0xFF) && (_p[i + 1] == 0xD9)) // JPEG trailer
            {
              found_FFD9 = true;
              ++i;
            }
            ++i;
          }
        }

        // Serial.printf("i: %d\n", i);
        memcpy(_mjpeg_buf + _mjpeg_buf_offset, _p, i); //메모디 버퍼와 오프셋에 _P 임시 구조체에 복사
        _mjpeg_buf_offset += i;
        size_t o = _buf_read - i;
        if (o > 0)
        {
          // Serial.printf("o: %d\n", o);
          memcpy(_read_buf, _p + i, o);
          _buf_read = _input->readBytes(_read_buf + o, READ_BUFFER_SIZE - o);
          _p = _read_buf;
          _inputindex += _buf_read;
          _buf_read += o;
          // Serial.printf("_buf_read: %d\n", _buf_read);
        }
        else
        {
          _buf_read = _input->readBytes(_read_buf, READ_BUFFER_SIZE);
          _p = _read_buf;
          _inputindex += _buf_read;
        }
        i = 0;
      }
      if (found_FFD9)
      {
        return true;
      }
    }

    return false;
  }

  int getWidth()
  {
    return _jpeg.getWidth();
  }

  int getHeight()
  {
    return _jpeg.getHeight();
  }

  bool drawJpg()
  {
    _remain = _mjpeg_buf_offset;

    if (_enableMultiTask)
    {
      _jpeg.openRAM(_mjpeg_buf, _remain, queueDrawMCU);
    }
    else
    {
      _jpeg.openRAM(_mjpeg_buf, _remain, _pfnDraw);
    }

    _jpeg.setMaxOutputSize(MAXOUTPUTSIZE);
    if (_useBigEndian)
    {
      _jpeg.setPixelType(RGB565_BIG_ENDIAN);
    }
    _jpeg.decode(0, 0, 0);
    _jpeg.close();

    return true;
  }

private:
  Stream *_input;
  uint8_t *_mjpeg_buf;
  JPEG_DRAW_CALLBACK *_pfnDraw;
  bool _enableMultiTask;
  bool _useBigEndian;

  uint8_t *_read_buf;
  int32_t _mjpeg_buf_offset;

  JPEGDEC _jpeg;
  paramDrawTask _p;

  int32_t _inputindex;
  int32_t _buf_read;
  int32_t _remain;
};

#endif // _MJPEGCLASS_H_
