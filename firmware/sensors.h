#pragma once

#include <Arduino.h>

/// Initialize IMU (MPU6886).
void sensors_init();

/// Read IMU and update internal state. Call from loop().
void sensors_update();

/// Get current orientation string.
const char* sensors_get_orientation();

/// Returns true if device is currently in motion.
bool sensors_is_moving();

/// Returns true if device is being shaken.
bool sensors_is_shaking();

/// Returns true if a tap was detected since last call (auto-clears).
bool sensors_tap_detected();

/// Get raw accelerometer values.
void sensors_get_accel(float& x, float& y, float& z);

/// Get raw gyroscope values.
void sensors_get_gyro(float& x, float& y, float& z);
