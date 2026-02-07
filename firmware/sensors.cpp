#include "sensors.h"
#include "config.h"
#include <M5Unified.h>

static float accel_x = 0, accel_y = 0, accel_z = 0;
static float gyro_x = 0, gyro_y = 0, gyro_z = 0;
static char  orientation[16] = "face_up";
static bool  moving = false;
static bool  shaking = false;
static bool  tap_flag = false;

// Smoothed values for motion detection
static float prev_accel_mag = 1.0f;

void sensors_init() {
    if (!M5.Imu.isEnabled()) {
        Serial.println("[IMU] IMU not available!");
        return;
    }
    Serial.println("[IMU] IMU initialized");
}

void sensors_update() {
    if (!M5.Imu.isEnabled()) return;

    auto data = M5.Imu.getImuData();
    accel_x = data.accel.x;
    accel_y = data.accel.y;
    accel_z = data.accel.z;
    gyro_x = data.gyro.x;
    gyro_y = data.gyro.y;
    gyro_z = data.gyro.z;

    // Calculate total acceleration magnitude
    float accel_mag = sqrtf(accel_x * accel_x + accel_y * accel_y + accel_z * accel_z);

    // Detect shaking (high g-force)
    shaking = accel_mag > SHAKE_THRESHOLD;

    // Detect motion (change in acceleration)
    float accel_diff = fabsf(accel_mag - prev_accel_mag);
    moving = accel_diff > 0.15f;
    prev_accel_mag = prev_accel_mag * 0.9f + accel_mag * 0.1f; // Smooth

    // Detect tap
    if (accel_mag > TAP_THRESHOLD && !shaking) {
        tap_flag = true;
    }

    // Determine orientation
    if (accel_z > 0.8f) {
        strcpy(orientation, "face_up");
    } else if (accel_z < -0.8f) {
        strcpy(orientation, "face_down");
    } else if (accel_x > 0.8f) {
        strcpy(orientation, "tilted_right");
    } else if (accel_x < -0.8f) {
        strcpy(orientation, "tilted_left");
    } else if (accel_y > 0.8f) {
        strcpy(orientation, "upright");
    } else if (accel_y < -0.8f) {
        strcpy(orientation, "upside_down");
    } else {
        strcpy(orientation, "angled");
    }
}

const char* sensors_get_orientation() {
    return orientation;
}

bool sensors_is_moving() {
    return moving;
}

bool sensors_is_shaking() {
    return shaking;
}

bool sensors_tap_detected() {
    if (tap_flag) {
        tap_flag = false;
        return true;
    }
    return false;
}

void sensors_get_accel(float& x, float& y, float& z) {
    x = accel_x;
    y = accel_y;
    z = accel_z;
}

void sensors_get_gyro(float& x, float& y, float& z) {
    x = gyro_x;
    y = gyro_y;
    z = gyro_z;
}
