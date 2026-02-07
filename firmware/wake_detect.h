#pragma once

#include <Arduino.h>

/// Initialize wake word detector (allocates circular buffer).
/// Call once during setup().
void wake_detect_init();

/// Start continuous listening for wake word.
/// Configures and starts the mic (stops speaker first).
void wake_detect_start();

/// Feed audio samples to the wake word detector.
/// Call from loop() while in wake listening state.
/// Returns true if wake word was detected.
///
/// Phase 1: Uses amplitude-based detection (loud sound triggers).
/// Phase 2: Swap in Edge Impulse TFLite Micro model for "Lo-Bug".
bool wake_detect_feed();

/// Stop wake word listening and release mic (I2S switch).
void wake_detect_stop();

/// Mark wake detector as inactive without stopping mic.
/// Used when seamlessly handing off to audio_capture.
void wake_detect_suspend();

/// Returns true if wake detector is actively listening.
bool wake_detect_is_listening();

/// Get pointer to the circular buffer data.
/// Used by audio_capture to copy buffered audio on wake detection.
const int16_t* wake_detect_get_buffer();

/// Get total capacity of the circular buffer (in samples).
size_t wake_detect_get_buffer_len();

/// Get current write position in the circular buffer.
size_t wake_detect_get_buffer_pos();

/// Get number of valid samples in the buffer (up to buffer_len).
size_t wake_detect_get_valid_samples();
