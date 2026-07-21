#pragma once

// Minimal QMI8658 accelerometer driver for tilt-based volume control.
// Only the accelerometer is used (gravity vector -> tilt angle), so the gyro is
// left disabled to save power and avoid its higher noise floor. Register map per
// the QMI8658A datasheet.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "config.h"

namespace imu {

namespace reg {
constexpr uint8_t WhoAmI = 0x00;  // -> 0x05
constexpr uint8_t Ctrl1 = 0x02;   // serial interface / address auto-increment
constexpr uint8_t Ctrl2 = 0x03;   // accel full-scale + ODR
constexpr uint8_t Ctrl5 = 0x06;   // accel low-pass filter
constexpr uint8_t Ctrl7 = 0x08;   // sensor enable
constexpr uint8_t AccelXL = 0x35;  // AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H (auto-increment)
}  // namespace reg

inline void writeReg(uint8_t r, uint8_t v) {
  Wire.beginTransmission(cfg::ImuI2cAddress);
  Wire.write(r);
  Wire.write(v);
  Wire.endTransmission();
}

inline uint8_t readReg(uint8_t r) {
  Wire.beginTransmission(cfg::ImuI2cAddress);
  Wire.write(r);
  Wire.endTransmission(false);
  Wire.requestFrom(static_cast<int>(cfg::ImuI2cAddress), 1);
  return Wire.available() ? Wire.read() : 0;
}

// Reads the three raw signed accelerometer axes. Only ratios matter for tilt, so
// the LSB/g scale factor is irrelevant and left unapplied. Returns false on any
// I2C fault so the caller can skip that sample.
inline bool readAccel(int16_t& ax, int16_t& ay, int16_t& az) {
  Wire.beginTransmission(cfg::ImuI2cAddress);
  Wire.write(reg::AccelXL);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(cfg::ImuI2cAddress), 6) != 6) {
    return false;
  }
  uint8_t b[6];
  for (int i = 0; i < 6; ++i) {
    b[i] = Wire.read();
  }
  ax = static_cast<int16_t>((b[1] << 8) | b[0]);  // little-endian (CTRL1 BE=0)
  ay = static_cast<int16_t>((b[3] << 8) | b[2]);
  az = static_cast<int16_t>((b[5] << 8) | b[4]);
  return true;
}

inline bool begin() {
  Wire.begin(cfg::PinI2cSda, cfg::PinI2cScl);
  Wire.setClock(400000);
  delay(10);

  uint8_t who = readReg(reg::WhoAmI);
  if (who != 0x05) {
    Serial.printf("[imu] WHO_AM_I=0x%02X (expected 0x05) - QMI8658 not found\n", who);
    return false;
  }

  writeReg(reg::Ctrl1, 0x40);  // address auto-increment, little-endian, I2C
  writeReg(reg::Ctrl2, 0x05);  // accel +-2g, ODR ~250 Hz
  writeReg(reg::Ctrl5, 0x03);  // accel low-pass filter enabled
  writeReg(reg::Ctrl7, 0x01);  // enable accelerometer only (gyro off)
  delay(20);

  Serial.println("[imu] QMI8658 ready");
  return true;
}

}  // namespace imu
