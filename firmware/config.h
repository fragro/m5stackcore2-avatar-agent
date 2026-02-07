#pragma once

// WiFi credentials
#define WIFI_SSID     "revolve"
#define WIFI_PASSWORD "fuck the police"

// Server configuration
#define SERVER_HOST   "192.168.50.244"  // Mac's local IP
#define SERVER_PORT   8321
#define SERVER_URL    "http://" SERVER_HOST ":" "8321"

// Audio settings
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16
#define CHANNELS          1
#define MAX_RECORD_SECS   5
#define AUDIO_BUFFER_SIZE (SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * CHANNELS * MAX_RECORD_SECS)

// VAD (Voice Activity Detection) settings
#define VAD_THRESHOLD     500   // Amplitude threshold for voice detection
#define VAD_SILENCE_MS    800   // Silence duration to stop recording

// Display settings
#define SCREEN_WIDTH      320
#define SCREEN_HEIGHT     240
#define STATUS_BAR_H      20
#define CHAT_AREA_Y       STATUS_BAR_H
#define CHAT_AREA_H       (SCREEN_HEIGHT - STATUS_BAR_H - 40)
#define BOTTOM_BAR_Y      (SCREEN_HEIGHT - 40)
#define BOTTOM_BAR_H      40
#define MSG_PADDING       4
#define MAX_MESSAGES       20

// Sensor settings
#define SENSOR_SEND_INTERVAL_MS  5000  // Send sensor data every 5 seconds
#define SHAKE_THRESHOLD          2.0f  // g-force threshold for shake detection
#define TAP_THRESHOLD            1.5f  // g-force threshold for tap detection

// Timeouts
#define HTTP_TIMEOUT_MS   30000
#define WIFI_TIMEOUT_MS   10000
