#pragma once

#include <Arduino.h>

/// Initialize speaker (called once at startup).
void audio_playback_init();

/// Play WAV data through the speaker.
/// Speaker must already be running (mic must be stopped).
void audio_playback_play(const uint8_t* wav_data, size_t wav_len);

/// Stop playback immediately.
void audio_playback_stop();

/// Returns true if audio is currently playing.
bool audio_playback_is_playing();

/// Get current playback audio level (0.0-1.0) for lip sync.
float audio_playback_get_level();

/// Set volume (0-255).
void audio_playback_set_volume(uint8_t vol);
