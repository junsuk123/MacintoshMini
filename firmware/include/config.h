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

constexpr BaseType_t AudioCore = 0;
constexpr BaseType_t DecodeCore = 0;
constexpr BaseType_t DrawCore = 1;

constexpr size_t FileNameLimit = 48;
constexpr size_t MaxPlaylistItems = 96;
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
