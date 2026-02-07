#include "audio_playback.h"
#include "config.h"
#include <M5Unified.h>

static const int16_t* play_pcm = nullptr;
static size_t play_total_samples = 0;
static size_t play_pos = 0;
static bool   playing = false;
static float  current_level = 0.0f;
static const size_t PLAY_CHUNK = 256;

void audio_playback_init() {
    M5.Speaker.setVolume(200);
    Serial.println("[SPK] Speaker initialized");
}

void audio_playback_play(const uint8_t* wav_data, size_t wav_len) {
    if (!wav_data || wav_len <= 44) return;

    // Parse sample rate from WAV header
    uint32_t wav_sample_rate;
    memcpy(&wav_sample_rate, wav_data + 24, 4);

    play_pcm = (const int16_t*)(wav_data + 44);
    play_total_samples = (wav_len - 44) / sizeof(int16_t);
    play_pos = 0;
    playing = true;

    // Ensure speaker is initialized (mic may have been using the I2S bus)
    M5.Speaker.begin();

    // Start playback using M5.Speaker
    M5.Speaker.playRaw(play_pcm, play_total_samples, wav_sample_rate, false, 1, 0);

    Serial.printf("[SPK] Playing %u samples at %uHz\n", play_total_samples, wav_sample_rate);
}

void audio_playback_stop() {
    playing = false;
    play_pcm = nullptr;
    current_level = 0.0f;
    M5.Speaker.stop();
}

bool audio_playback_is_playing() {
    if (playing) {
        // Check if M5.Speaker finished
        if (!M5.Speaker.isPlaying()) {
            playing = false;
            current_level = 0.0f;
        }
    }
    return playing;
}

float audio_playback_get_level() {
    if (!playing || !play_pcm) return 0.0f;

    // Estimate current playback position based on elapsed time
    // and compute level from the PCM data around that position
    size_t check_pos = play_pos;
    if (check_pos + PLAY_CHUNK > play_total_samples) {
        if (play_total_samples > PLAY_CHUNK) {
            check_pos = play_total_samples - PLAY_CHUNK;
        } else {
            check_pos = 0;
        }
    }

    int64_t sum_sq = 0;
    size_t count = (play_total_samples - check_pos < PLAY_CHUNK)
                 ? (play_total_samples - check_pos) : PLAY_CHUNK;
    for (size_t i = 0; i < count; i++) {
        int32_t s = play_pcm[check_pos + i];
        sum_sq += (int64_t)s * s;
    }

    float rms = sqrtf((float)sum_sq / count);
    current_level = fminf(1.0f, rms / 10000.0f);

    // Advance position estimate
    play_pos += PLAY_CHUNK;
    if (play_pos >= play_total_samples) {
        play_pos = play_total_samples;
    }

    return current_level;
}

void audio_playback_set_volume(uint8_t vol) {
    M5.Speaker.setVolume(vol);
}
