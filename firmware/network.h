#pragma once

#include <Arduino.h>

/// Initialize WiFi connection. Blocks until connected or timeout.
/// Returns true if connected.
bool network_init();

/// Check if WiFi is connected.
bool network_is_connected();

/// Call the /health endpoint. Returns true if server is reachable.
bool network_health_check();

/// POST text to /chat/text. Returns the response string.
/// On failure, returns an empty string.
String network_chat_text(const String& text);

/// POST WAV audio to /chat/audio.
/// Returns true on success, populating transcription, response, and audio output buffer.
bool network_chat_audio(const uint8_t* wav_data, size_t wav_len,
                        String& transcription, String& response,
                        uint8_t** audio_out, size_t* audio_out_len);

/// POST sensor data to /context/sensors.
void network_send_sensors(float accel_x, float accel_y, float accel_z,
                          float gyro_x, float gyro_y, float gyro_z,
                          const char* orientation, bool is_moving,
                          bool is_shaking, bool tap_detected);
