#include "driver/i2s.h"

#include "AACDecoderHelix.h"
#include "MP3DecoderHelix.h"

static unsigned long total_read_audio_ms = 0;
static unsigned long total_decode_audio_ms = 0;
static unsigned long total_play_audio_ms = 0;
struct TaskParameters{//오디오 태스크에 여러개의 인자를 전달하기 위한 구조체
    Stream *input;
};
static i2s_port_t _i2s_num;
static esp_err_t i2s_init(i2s_port_t i2s_num, uint32_t sample_rate,
                          int mck_io_num,   /*!< MCK in out pin. Note that ESP32 supports setting MCK on GPIO0/GPIO1/GPIO3 only*/
                          int bck_io_num,   /*!< BCK in out pin*/
                          int ws_io_num,    /*!< WS in out pin*/
                          int data_out_num, /*!< DATA out pin*/
                          int data_in_num   /*!< DATA in pin*/
)
{
    _i2s_num = i2s_num;

    esp_err_t ret_val = ESP_OK;

    i2s_config_t i2s_config;
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = sample_rate;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 160;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;
    i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
    i2s_config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pin_config;
    pin_config.mck_io_num = mck_io_num;
    pin_config.bck_io_num = bck_io_num;
    pin_config.ws_io_num = ws_io_num;
    pin_config.data_out_num = data_out_num;
    pin_config.data_in_num = data_in_num;

    ret_val |= i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
    ret_val |= i2s_set_pin(i2s_num, &pin_config);

    return ret_val;
}

static int _samprate = 0;
static void aacAudioDataCallback(AACFrameInfo &info, int16_t *pwm_buffer, size_t len, void*)
{
    unsigned long s = millis();
    if (_samprate != info.sampRateOut)
    {
        i2s_set_clk(_i2s_num, info.sampRateOut /* sample_rate */, info.bitsPerSample /* bits_cfg */, (info.nChans == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO /* channel */);
        _samprate = info.sampRateOut;
    }
    size_t i2s_bytes_written = 0;
    i2s_write(_i2s_num, pwm_buffer, len * 2, &i2s_bytes_written, portMAX_DELAY);
    total_play_audio_ms += millis() - s;
}

static uint8_t _frame[MP3_MAX_FRAME_SIZE]; // MP3_MAX_FRAME_SIZE is smaller, so always use MP3_MAX_FRAME_SIZE

static libhelix::AACDecoderHelix _aac(aacAudioDataCallback);
static void aac_player_task(void *pvParam) {
    TaskParameters *taskParams = (TaskParameters *)pvParam;//인자 구조체 선언
    Stream *input = taskParams->input;// 오디오 파일 주소
    bool isPlaying=true;
    unsigned long startPlaybackTime = 0; // 오디오 재생 시작 시간 기록

    int r, w;// r: 현재까지 남은 오디오 프레임 수, w: 출력한 오디오 프레임 수
    unsigned long ms = millis();

    while (r = input->readBytes(_frame, MP3_MAX_FRAME_SIZE)) {
        // 현재 버튼 상태를 읽어옴 (버튼 대신 isPause 값을 확인)
        bool isClicked =(digitalRead(17) == HIGH);
        if(isClicked&&isPlaying){//일시정지
            isPlaying = false;//재생 상태를 일시 정지로 바꿈
            Serial.println("Pause audio!!");
            delay(200);
            startPlaybackTime = 0;
        }
        else if(isClicked&&!isPlaying){//영상 재생 재개
          isPlaying = true;//재생 상태를 재생으로 바꿈
          Serial.println("Resume audio!!");
          delay(200);
          startPlaybackTime = millis();
        }

        // 오디오 재생 중일 때만 처리
        if (isPlaying) {//재생상태가 true일 때
            if (startPlaybackTime == 0) {//일시 정지 혹은 영상 재생 시작때에는 SPT가 0이 됨->코드 자체에는 아무런 영향이 없음.
                startPlaybackTime = millis();
            }

            // 오디오 데이터를 디코딩하고 재생
            while (r > 0) {
                w = _aac.write(_frame, r);
                r -= w;
            }

            total_decode_audio_ms += millis() - ms;
            ms = millis();
        }
    }

    // 재생이 끝나면 상태 초기화 및 작업 종료
    log_i("AAC stop.");
    vTaskDelete(NULL);
}

static BaseType_t aac_player_task_start(Stream *input, BaseType_t audioAssignCore)
{
    _aac.begin();
    TaskParameters *taskParams = new TaskParameters(); // 동적으로 메모리 할당

    taskParams->input = input;

    BaseType_t result = xTaskCreatePinnedToCore(
        (TaskFunction_t)aac_player_task,
        "AAC Player Task",
        2000,
        taskParams, // 구조체의 주소를 전달
        configMAX_PRIORITIES - 1,
        NULL,
        audioAssignCore);

    if (result != pdPASS) {
        delete taskParams; // 메모리 해제
    }

    return result;
}

