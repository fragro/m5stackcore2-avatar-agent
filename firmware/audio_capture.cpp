#include "audio_capture.h"
#include "wake_detect.h"
#include "config.h"
#include <M5Unified.h>

// Recording buffer in PSRAM
static uint8_t* wav_buffer = nullptr;
static size_t   pcm_offset = 0;     // Current write position in PCM data (after header)
static bool     recording = false;
static float    current_level = 0.0f;

// VAD state
static unsigned long silence_start = 0;
static bool          voice_detected = false;
static unsigned long record_start_time = 0;

// Chunk buffer for M5.Mic.record()
static const size_t CHUNK_SAMPLES = 512;
static int16_t chunk_buf[CHUNK_SAMPLES];

static void write_wav_header(uint8_t* buf, uint32_t data_size) {
    uint32_t file_size = data_size + 36;
    uint32_t byte_rate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    uint16_t block_align = CHANNELS * (BITS_PER_SAMPLE / 8);

    memcpy(buf, "RIFF", 4);
    memcpy(buf + 4, &file_size, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_format = 1;
    memcpy(buf + 20, &audio_format, 2);
    uint16_t channels = CHANNELS;
    memcpy(buf + 22, &channels, 2);
    uint32_t sr = SAMPLE_RATE;
    memcpy(buf + 24, &sr, 4);
    memcpy(buf + 28, &byte_rate, 4);
    memcpy(buf + 32, &block_align, 2);
    uint16_t bps = BITS_PER_SAMPLE;
    memcpy(buf + 34, &bps, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);
}

void audio_capture_init() {
    wav_buffer = (uint8_t*)heap_caps_malloc(44 + AUDIO_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!wav_buffer) {
        Serial.println("[MIC] Failed to allocate WAV buffer!");
        return;
    }
    Serial.println("[MIC] Audio capture initialized");
}

void audio_capture_start() {
    if (!wav_buffer) return;

    pcm_offset = 0;
    voice_detected = false;
    silence_start = 0;
    current_level = 0.0f;
    recording = true;
    record_start_time = millis();

    // CRITICAL: Stop speaker before starting mic (shared I2S bus)
    M5.Speaker.end();

    // Configure and start microphone
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.magnification = 24;  // Boost mic level (default 16)
    mic_cfg.noise_filter_level = 64;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();

    Serial.println("[MIC] Recording started");
}

void audio_capture_start_from_wake() {
    if (!wav_buffer) return;

    pcm_offset = 0;
    voice_detected = false;  // Let VAD detect the actual command speech naturally
    silence_start = 0;
    current_level = 0.0f;
    recording = true;
    record_start_time = millis();

    // Mic is ALREADY running from wake detection — no I2S switch needed.
    // Mark wake detector as suspended (we're taking over the mic)
    wake_detect_suspend();

    // Note: we intentionally do NOT copy the wake circular buffer here.
    // The buffer contains the wake trigger sound + ambient noise, which
    // isn't useful for STT. The user's actual command comes AFTER the
    // wake word, so we start recording fresh from this point.
    // The circular buffer infrastructure remains for Phase 2 where a real
    // wake word model may need it for context.

    Serial.println("[MIC] Recording started (from wake, fresh buffer)");
}

void audio_capture_stop() {
    if (!recording) return;
    recording = false;

    // Wait for any in-progress recording to finish
    while (M5.Mic.isRecording()) { delay(1); }

    // Stop mic only — speaker will be restarted by whoever needs it next
    // (audio_playback or wake_detect_stop). This avoids I2S double-init errors.
    M5.Mic.end();

    // Write WAV header with actual data size
    write_wav_header(wav_buffer, pcm_offset);

    Serial.printf("[MIC] Recording stopped. %u bytes PCM captured\n", pcm_offset);
}

bool audio_capture_is_recording() {
    return recording;
}

bool audio_capture_update() {
    if (!recording || !wav_buffer) return false;

    // Check max recording time
    if (millis() - record_start_time > (MAX_RECORD_SECS * 1000)) {
        return true;
    }

    // Check buffer space
    size_t bytes_needed = CHUNK_SAMPLES * sizeof(int16_t);
    if (pcm_offset + bytes_needed > AUDIO_BUFFER_SIZE) {
        return true;
    }

    // Record a chunk using M5.Mic
    if (M5.Mic.record(chunk_buf, CHUNK_SAMPLES, SAMPLE_RATE)) {
        // Copy to WAV buffer (after 44-byte header)
        memcpy(wav_buffer + 44 + pcm_offset, chunk_buf, bytes_needed);
        pcm_offset += bytes_needed;

        // Calculate amplitude for VAD and lip sync
        int32_t max_amp = 0;
        int64_t sum_sq = 0;
        for (size_t i = 0; i < CHUNK_SAMPLES; i++) {
            int32_t amp = abs((int32_t)chunk_buf[i]);
            if (amp > max_amp) max_amp = amp;
            sum_sq += (int64_t)chunk_buf[i] * chunk_buf[i];
        }

        // RMS level for lip sync (0.0 - 1.0)
        float rms = sqrtf((float)sum_sq / CHUNK_SAMPLES);
        current_level = fminf(1.0f, rms / 8000.0f);

        // VAD: check if voice is present
        if (max_amp > VAD_THRESHOLD) {
            voice_detected = true;
            silence_start = 0;
        } else if (voice_detected) {
            if (silence_start == 0) {
                silence_start = millis();
            } else if (millis() - silence_start > VAD_SILENCE_MS) {
                return true; // Voice detected then silence = done
            }
        }
    }

    return false;
}

const uint8_t* audio_capture_get_wav() {
    return wav_buffer;
}

size_t audio_capture_get_wav_size() {
    return 44 + pcm_offset;
}

float audio_capture_get_level() {
    return current_level;
}
