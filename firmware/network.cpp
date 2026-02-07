#include "network.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Base64 decode table
static const uint8_t b64_table[] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,0,64,64,
    64,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
};

static size_t base64_decode(const char* input, size_t input_len, uint8_t* output) {
    size_t out_len = 0;
    uint32_t buf = 0;
    int bits = 0;

    for (size_t i = 0; i < input_len; i++) {
        uint8_t c = (uint8_t)input[i];
        if (c == '=' || c >= 128) continue;
        uint8_t val = b64_table[c];
        if (val == 64) continue;

        buf = (buf << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output[out_len++] = (buf >> bits) & 0xFF;
        }
    }
    return out_len;
}

bool network_init() {
    Serial.println("[NET] Connecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            Serial.println("[NET] WiFi timeout!");
            return false;
        }
        delay(250);
    }

    Serial.printf("[NET] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool network_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

bool network_health_check() {
    HTTPClient http;
    String url = String(SERVER_URL) + "/health";
    http.begin(url);
    http.setTimeout(5000);

    int code = http.GET();
    http.end();

    return code == 200;
}

String network_chat_text(const String& text) {
    HTTPClient http;
    String url = String(SERVER_URL) + "/chat/text";
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["text"] = text;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    String result;

    if (code == 200) {
        String payload = http.getString();
        StaticJsonDocument<4096> resp;
        if (deserializeJson(resp, payload) == DeserializationError::Ok) {
            result = resp["response"].as<String>();
        }
    } else {
        Serial.printf("[NET] /chat/text failed: %d\n", code);
    }

    http.end();
    return result;
}

bool network_chat_audio(const uint8_t* wav_data, size_t wav_len,
                        String& transcription, String& response,
                        uint8_t** audio_out, size_t* audio_out_len) {
    HTTPClient http;
    String url = String(SERVER_URL) + "/chat/audio";
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);

    // Build multipart form data
    String boundary = "----M5StackBoundary";
    String content_type = "multipart/form-data; boundary=" + boundary;
    http.addHeader("Content-Type", content_type);

    // Build body
    String header_part = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String footer_part = "\r\n--" + boundary + "--\r\n";

    size_t total_len = header_part.length() + wav_len + footer_part.length();
    uint8_t* body = (uint8_t*)ps_malloc(total_len);
    if (!body) {
        Serial.println("[NET] Failed to allocate multipart buffer");
        return false;
    }

    size_t offset = 0;
    memcpy(body + offset, header_part.c_str(), header_part.length());
    offset += header_part.length();
    memcpy(body + offset, wav_data, wav_len);
    offset += wav_len;
    memcpy(body + offset, footer_part.c_str(), footer_part.length());

    int code = http.POST(body, total_len);
    free(body);

    if (code != 200) {
        Serial.printf("[NET] /chat/audio failed: %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Parse JSON response
    DynamicJsonDocument doc(payload.length() + 1024);
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        Serial.println("[NET] Failed to parse audio response JSON");
        return false;
    }

    transcription = doc["transcription"].as<String>();
    response = doc["response"].as<String>();

    // Decode base64 audio
    const char* b64 = doc["audio_b64"].as<const char*>();
    if (b64) {
        size_t b64_len = strlen(b64);
        size_t max_decoded = (b64_len * 3) / 4 + 4;
        *audio_out = (uint8_t*)ps_malloc(max_decoded);
        if (*audio_out) {
            *audio_out_len = base64_decode(b64, b64_len, *audio_out);
        } else {
            *audio_out_len = 0;
            Serial.println("[NET] Failed to allocate audio decode buffer");
        }
    } else {
        *audio_out = nullptr;
        *audio_out_len = 0;
    }

    return true;
}

void network_send_sensors(float accel_x, float accel_y, float accel_z,
                          float gyro_x, float gyro_y, float gyro_z,
                          const char* orientation, bool is_moving,
                          bool is_shaking, bool tap_detected) {
    HTTPClient http;
    String url = String(SERVER_URL) + "/context/sensors";
    http.begin(url);
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["accel_x"] = accel_x;
    doc["accel_y"] = accel_y;
    doc["accel_z"] = accel_z;
    doc["gyro_x"] = gyro_x;
    doc["gyro_y"] = gyro_y;
    doc["gyro_z"] = gyro_z;
    doc["orientation"] = orientation;
    doc["is_moving"] = is_moving;
    doc["is_shaking"] = is_shaking;
    doc["tap_detected"] = tap_detected;

    String body;
    serializeJson(doc, body);
    http.POST(body);
    http.end();
}
