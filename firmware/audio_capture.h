#pragma once

#include <Arduino.h>

/// Initialize microphone via M5Unified Mic_Class.
void audio_capture_init();

/// Start recording audio into PSRAM buffer.
/// Stops the speaker first (shared I2S bus).
void audio_capture_start();

/// Stop recording and finalize WAV buffer.
/// Restarts the speaker after stopping mic.
void audio_capture_stop();

/// Returns true if currently recording.
bool audio_capture_is_recording();

/// Record audio samples. Call from loop() while recording.
/// Returns true if VAD detected end-of-speech or max time reached.
bool audio_capture_update();

/// Get pointer to the recorded WAV data.
const uint8_t* audio_capture_get_wav();

/// Get size of the recorded WAV data in bytes.
size_t audio_capture_get_wav_size();

/// Get current audio level (0.0-1.0) for lip sync during recording.
float audio_capture_get_level();
