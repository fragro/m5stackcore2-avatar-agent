/*
 * M5Stack Core2 Intelligent Communicative Agent
 *
 * Cute animated avatar face with local AI voice/text assistant.
 * Uses M5Stack-Avatar for face display, M5Unified Mic_Class for audio.
 * Server runs Whisper (STT), Ollama (LLM), and Piper (TTS) locally.
 *
 * Controls:
 *   BtnA (left)   = Hold to talk
 *   BtnB (center) = Type message (keyboard)
 *   BtnC (right)  = Menu / close keyboard
 */

#include <M5Unified.h>
#include <Avatar.h>
#include "config.h"
#include "network.h"
#include "display_ui.h"
#include "audio_capture.h"
#include "audio_playback.h"
#include "sensors.h"

using namespace m5avatar;

enum AppState {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_IDLE,
    STATE_RECORDING,
    STATE_SENDING_AUDIO,
    STATE_SENDING_TEXT,
    STATE_PLAYING_RESPONSE,
    STATE_KEYBOARD,
};

static AppState state = STATE_INIT;
static bool wifi_connected = false;
static bool server_connected = false;
static unsigned long last_sensor_send = 0;
static unsigned long last_health_check = 0;

// Response audio buffer
static uint8_t* response_audio = nullptr;
static size_t   response_audio_len = 0;

static void free_response_audio() {
    if (response_audio) {
        free(response_audio);
        response_audio = nullptr;
        response_audio_len = 0;
    }
}

static void transition(AppState new_state) {
    Serial.printf("[APP] State: %d -> %d\n", state, new_state);
    state = new_state;
}

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_spk = true;
    cfg.internal_mic = true;
    M5.begin(cfg);

    Serial.begin(115200);
    Serial.println("\n=== M5Stack Intelligent Agent ===");

    // Initialize avatar face first (gives user something to look at)
    display_init();
    display_set_expression(Expression::Neutral);
    display_set_speech("Starting...");

    audio_capture_init();
    audio_playback_init();
    sensors_init();

    transition(STATE_CONNECTING);
}

void loop() {
    M5.update();

    switch (state) {
        case STATE_CONNECTING:  handle_connecting();     break;
        case STATE_IDLE:        handle_idle();           break;
        case STATE_RECORDING:   handle_recording();      break;
        case STATE_SENDING_AUDIO: handle_sending_audio(); break;
        case STATE_SENDING_TEXT:  handle_sending_text();  break;
        case STATE_PLAYING_RESPONSE: handle_playing();   break;
        case STATE_KEYBOARD:    handle_keyboard();       break;
        default: break;
    }

    // Periodic tasks
    if (state == STATE_IDLE || state == STATE_KEYBOARD) {
        handle_sensors();
        handle_health_check();
    }

    delay(10);
}

// --- State Handlers ---

void handle_connecting() {
    display_set_expression(Expression::Doubt);
    display_set_speech("Connecting WiFi...");

    wifi_connected = network_init();
    if (!wifi_connected) {
        display_set_expression(Expression::Sad);
        display_set_speech("WiFi failed!");
        delay(3000);
        return;
    }

    display_set_speech("Finding server...");

    server_connected = network_health_check();
    if (server_connected) {
        display_set_expression(Expression::Happy);
        display_set_speech("Connected!");
        delay(1500);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
    } else {
        display_set_expression(Expression::Sad);
        display_set_speech("No server found");
        delay(2000);
        display_set_speech("");
        display_set_expression(Expression::Sleepy);
    }

    transition(STATE_IDLE);
}

void handle_idle() {
    int btn = display_check_buttons();

    switch (btn) {
        case 1: // BtnA = Talk
            if (!server_connected) {
                display_set_expression(Expression::Sad);
                display_set_speech("No server!");
                delay(1000);
                display_set_speech("");
                display_set_expression(Expression::Neutral);
                break;
            }
            display_set_expression(Expression::Happy);
            display_set_speech("Listening...");
            audio_capture_start();
            transition(STATE_RECORDING);
            break;

        case 2: // BtnB = Type
            if (!server_connected) {
                display_set_expression(Expression::Sad);
                display_set_speech("No server!");
                delay(1000);
                display_set_speech("");
                display_set_expression(Expression::Neutral);
                break;
            }
            display_keyboard_open();
            transition(STATE_KEYBOARD);
            break;

        case 3: // BtnC = Menu
            display_set_expression(Expression::Happy);
            display_set_speech("I'm your local AI!");
            delay(2000);
            display_set_speech("");
            display_set_expression(Expression::Neutral);
            break;
    }
}

void handle_recording() {
    bool should_stop = audio_capture_update();

    // Lip sync: mirror mic level on avatar mouth
    float level = audio_capture_get_level();
    display_set_mouth(level);

    // Stop on BtnA press or VAD silence
    if (M5.BtnA.wasPressed()) {
        should_stop = true;
    }

    if (should_stop) {
        audio_capture_stop();
        display_set_mouth(0);
        display_set_expression(Expression::Doubt);
        display_set_speech("Thinking...");
        transition(STATE_SENDING_AUDIO);
    }
}

void handle_sending_audio() {
    const uint8_t* wav = audio_capture_get_wav();
    size_t wav_len = audio_capture_get_wav_size();

    if (wav_len <= 44) {
        display_set_expression(Expression::Sad);
        display_set_speech("Didn't hear anything");
        delay(1500);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
        transition(STATE_IDLE);
        return;
    }

    String transcription, response;
    free_response_audio();

    bool ok = network_chat_audio(wav, wav_len, transcription, response,
                                  &response_audio, &response_audio_len);

    if (ok) {
        Serial.printf("[APP] You said: %s\n", transcription.c_str());
        Serial.printf("[APP] Response: %s\n", response.c_str());

        display_set_expression(Expression::Happy);

        // Show response text briefly
        // Truncate for speech balloon
        String short_resp = response;
        if (short_resp.length() > 60) {
            short_resp = short_resp.substring(0, 57) + "...";
        }
        display_set_speech(short_resp.c_str());

        // Play response audio with lip sync
        if (response_audio && response_audio_len > 44) {
            audio_playback_play(response_audio, response_audio_len);
            transition(STATE_PLAYING_RESPONSE);
            return;
        }

        delay(3000);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
    } else {
        display_set_expression(Expression::Sad);
        display_set_speech("Error!");
        delay(1500);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
    }

    transition(STATE_IDLE);
}

void handle_sending_text() {
    // This state is entered from keyboard after send
    // The actual network call happens inline in handle_keyboard
    transition(STATE_IDLE);
}

void handle_playing() {
    if (audio_playback_is_playing()) {
        // Lip sync during playback
        float level = audio_playback_get_level();
        display_set_mouth(level);
    } else {
        // Playback finished
        display_set_mouth(0);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
        free_response_audio();
        transition(STATE_IDLE);
    }

    // Allow interrupting with any button
    int btn = display_check_buttons();
    if (btn > 0) {
        audio_playback_stop();
        display_set_mouth(0);
        display_set_speech("");
        display_set_expression(Expression::Neutral);
        free_response_audio();
        transition(STATE_IDLE);
    }
}

void handle_keyboard() {
    String typed_text;
    if (display_keyboard_update(typed_text)) {
        // User pressed Send
        display_keyboard_close();

        display_set_expression(Expression::Doubt);
        display_set_speech("Thinking...");

        String response = network_chat_text(typed_text);

        if (response.length() > 0) {
            display_set_expression(Expression::Happy);
            String short_resp = response;
            if (short_resp.length() > 60) {
                short_resp = short_resp.substring(0, 57) + "...";
            }
            display_set_speech(short_resp.c_str());
            delay(4000);
        } else {
            display_set_expression(Expression::Sad);
            display_set_speech("No response");
            delay(1500);
        }

        display_set_speech("");
        display_set_expression(Expression::Neutral);
        transition(STATE_IDLE);
        return;
    }

    // BtnC closes keyboard
    if (M5.BtnC.wasPressed()) {
        display_keyboard_close();
        transition(STATE_IDLE);
    }
}

void handle_sensors() {
    sensors_update();

    unsigned long now = millis();
    if (now - last_sensor_send >= SENSOR_SEND_INTERVAL_MS) {
        last_sensor_send = now;

        float ax, ay, az, gx, gy, gz;
        sensors_get_accel(ax, ay, az);
        sensors_get_gyro(gx, gy, gz);

        // Make avatar gaze follow tilt direction
        float gaze_h = constrain(ax * 0.5f, -1.0f, 1.0f);
        float gaze_v = constrain(ay * 0.5f, -1.0f, 1.0f);
        display_set_gaze(gaze_v, gaze_h);

        // React to shaking
        if (sensors_is_shaking()) {
            display_set_expression(Expression::Angry);
            display_set_speech("Hey! Stop shaking!");
            delay(1000);
            display_set_speech("");
            display_set_expression(Expression::Neutral);
        }

        network_send_sensors(
            ax, ay, az, gx, gy, gz,
            sensors_get_orientation(),
            sensors_is_moving(),
            sensors_is_shaking(),
            sensors_tap_detected()
        );
    }
}

void handle_health_check() {
    unsigned long now = millis();
    if (now - last_health_check >= 30000) {
        last_health_check = now;

        bool was_connected = server_connected;
        wifi_connected = network_is_connected();
        if (wifi_connected) {
            server_connected = network_health_check();
        } else {
            server_connected = false;
        }

        // Update face when connection state changes
        if (server_connected && !was_connected) {
            display_set_expression(Expression::Happy);
            display_set_speech("Connected!");
            delay(1500);
            display_set_speech("");
            display_set_expression(Expression::Neutral);
        } else if (!server_connected && was_connected) {
            display_set_expression(Expression::Sad);
            display_set_speech("Lost server...");
            delay(1500);
            display_set_speech("");
            display_set_expression(Expression::Sleepy);
        }
    }
}
