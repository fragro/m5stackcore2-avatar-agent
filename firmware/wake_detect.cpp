#include "wake_detect.h"
#include "config.h"
#include <M5Unified.h>

// Circular buffer for continuous audio (allocated in PSRAM)
static int16_t* circ_buf = nullptr;
static size_t   circ_pos = 0;        // Current write position
static size_t   circ_valid = 0;      // Number of valid samples written

static bool listening = false;

// Chunk buffer for reading mic samples
static const size_t WAKE_CHUNK_SAMPLES = WAKE_FRAME_SAMPLES;  // 480 samples = 30ms
static int16_t wake_chunk[480];  // Stack-allocated frame buffer

// --- Phase 1: Amplitude-based wake detection ---
// Detects a sustained loud sound as a stand-in for a real wake word.
// Replace this section with TFLite Micro inference for Phase 2.

// Detection requires both peak amplitude AND RMS energy above thresholds
// for several consecutive frames. Voice has sustained energy across a frame;
// impulse noise (door slams) has high peak but low RMS.
static const int16_t WAKE_AMP_THRESHOLD = 3000;    // Peak amplitude threshold (noise floor ~500)
static const float   WAKE_RMS_THRESHOLD = 1500.0f; // RMS energy threshold
static const int     WAKE_FRAMES_NEEDED = 4;        // Consecutive frames above threshold (~120ms)
static int           wake_frames_above = 0;

// Cooldown: ignore for a short period after detection to prevent re-trigger
static unsigned long wake_cooldown_until = 0;
static const unsigned long WAKE_COOLDOWN_MS = 2000;

// Grace period: skip detection for first N frames after mic starts
// (I2S bus switch from speaker creates transient noise)
static const int WAKE_GRACE_FRAMES = 10;  // ~300ms at 30ms/frame
static int       wake_grace_remaining = 0;

// Debug: log amplitude periodically
static unsigned long last_amp_log = 0;

static bool detect_wake_amplitude(const int16_t* samples, size_t count) {
    // Compute peak amplitude and RMS energy in one pass
    int32_t max_amp = 0;
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)samples[i];
        int32_t amp = abs(s);
        if (amp > max_amp) max_amp = amp;
        sum_sq += s * s;
    }
    float rms = sqrtf((float)sum_sq / (float)count);

    // Both peak AND RMS must exceed thresholds
    if (max_amp > WAKE_AMP_THRESHOLD && rms > WAKE_RMS_THRESHOLD) {
        wake_frames_above++;
        if (wake_frames_above >= WAKE_FRAMES_NEEDED) {
            wake_frames_above = 0;
            return true;
        }
    } else {
        wake_frames_above = 0;
    }

    return false;
}

// --- End Phase 1 detector ---

void wake_detect_init() {
    circ_buf = (int16_t*)heap_caps_malloc(
        WAKE_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);

    if (!circ_buf) {
        Serial.println("[WAKE] Failed to allocate circular buffer!");
        return;
    }

    memset(circ_buf, 0, WAKE_BUF_SAMPLES * sizeof(int16_t));
    Serial.printf("[WAKE] Initialized, buffer=%u samples (%u bytes)\n",
                  WAKE_BUF_SAMPLES, WAKE_BUF_SAMPLES * sizeof(int16_t));
}

void wake_detect_start() {
    if (!circ_buf) return;

    circ_pos = 0;
    circ_valid = 0;
    wake_frames_above = 0;
    wake_grace_remaining = WAKE_GRACE_FRAMES;
    listening = true;

    // Stop speaker, start mic (shared I2S bus)
    M5.Speaker.end();

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.magnification = 24;
    mic_cfg.noise_filter_level = 64;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();

    Serial.println("[WAKE] Listening started");
}

void wake_detect_stop() {
    if (!listening) return;
    listening = false;

    while (M5.Mic.isRecording()) { delay(1); }
    M5.Mic.end();
    M5.Speaker.begin();

    Serial.println("[WAKE] Listening stopped");
}

void wake_detect_suspend() {
    // Mark as not listening, but don't touch I2S hardware.
    // The mic stays running for audio_capture to take over.
    listening = false;
    Serial.println("[WAKE] Suspended (mic handed off)");
}

bool wake_detect_is_listening() {
    return listening;
}

bool wake_detect_feed() {
    if (!listening || !circ_buf) return false;

    // Read a frame from mic
    if (!M5.Mic.record(wake_chunk, WAKE_CHUNK_SAMPLES, SAMPLE_RATE)) {
        return false;
    }

    // Write into circular buffer (always, even during grace/cooldown)
    for (size_t i = 0; i < WAKE_CHUNK_SAMPLES; i++) {
        circ_buf[circ_pos] = wake_chunk[i];
        circ_pos = (circ_pos + 1) % WAKE_BUF_SAMPLES;
    }
    if (circ_valid < WAKE_BUF_SAMPLES) {
        circ_valid += WAKE_CHUNK_SAMPLES;
        if (circ_valid > WAKE_BUF_SAMPLES) circ_valid = WAKE_BUF_SAMPLES;
    }

    // Grace period: skip detection for first frames after mic starts
    if (wake_grace_remaining > 0) {
        wake_grace_remaining--;
        return false;
    }

    // Cooldown: skip detection briefly after a trigger
    if (millis() < wake_cooldown_until) {
        return false;
    }

    // Debug: log peak amplitude and RMS every 2 seconds
    int32_t frame_max = 0;
    int64_t frame_sum_sq = 0;
    for (size_t i = 0; i < WAKE_CHUNK_SAMPLES; i++) {
        int32_t s = (int32_t)wake_chunk[i];
        int32_t amp = abs(s);
        if (amp > frame_max) frame_max = amp;
        frame_sum_sq += s * s;
    }
    float frame_rms = sqrtf((float)frame_sum_sq / (float)WAKE_CHUNK_SAMPLES);
    if (millis() - last_amp_log > 2000) {
        Serial.printf("[WAKE] peak=%d rms=%.0f (thresholds: peak=%d rms=%.0f)\n",
                      frame_max, frame_rms, WAKE_AMP_THRESHOLD, WAKE_RMS_THRESHOLD);
        last_amp_log = millis();
    }

    // Run detection on this frame
    // Phase 1: amplitude-based, Phase 2: replace with TFLite inference
    bool detected = detect_wake_amplitude(wake_chunk, WAKE_CHUNK_SAMPLES);

    if (detected) {
        wake_cooldown_until = millis() + WAKE_COOLDOWN_MS;
        Serial.printf("[WAKE] >>> Wake detected! <<< (peak=%d rms=%.0f)\n", frame_max, frame_rms);
        return true;
    }

    return false;
}

const int16_t* wake_detect_get_buffer() {
    return circ_buf;
}

size_t wake_detect_get_buffer_len() {
    return WAKE_BUF_SAMPLES;
}

size_t wake_detect_get_buffer_pos() {
    return circ_pos;
}

size_t wake_detect_get_valid_samples() {
    return circ_valid;
}
