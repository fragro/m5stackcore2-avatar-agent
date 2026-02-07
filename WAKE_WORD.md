# Wake Word Detection -- Design & Integration Guide

This document covers the wake word detection system built into the M5Stack Core2
voice assistant. It explains the architecture, every tunable parameter, the API
surface, and a concrete roadmap for upgrading from the current amplitude-based
detector to a trained Edge Impulse TFLite Micro model.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
   - [State Machine](#21-state-machine)
   - [I2S Bus Management](#22-i2s-bus-management)
   - [Seamless Mic Handoff](#23-seamless-mic-handoff)
   - [Circular Buffer Design](#24-circular-buffer-design)
3. [Module Reference](#3-module-reference)
   - [wake_detect.h](#31-wake_detecth)
   - [audio_capture -- start_from_wake()](#32-audio_capture--audio_capture_start_from_wake)
   - [display_ui -- display_wake_listening_update()](#33-display_ui--display_wake_listening_update)
4. [Configuration](#4-configuration)
5. [Phase 2 Guide -- Edge Impulse TFLite Micro](#5-phase-2-guide--edge-impulse-tflite-micro)
   - [Recording Training Data](#51-recording-training-data)
   - [Training on Edge Impulse](#52-training-on-edge-impulse)
   - [Exporting the Model](#53-exporting-the-model)
   - [Integrating into wake_detect.cpp](#54-integrating-into-wake_detectcpp)
   - [Adding the Library to platformio.ini](#55-adding-the-library-to-platformioini)
6. [Why Not ESP-SR / WakeNet?](#6-why-not-esp-sr--wakenet)
7. [Troubleshooting](#7-troubleshooting)
8. [Files Modified / Created](#8-files-modified--created)

---

## 1. Overview

The voice assistant runs entirely hands-free. When the device is idle it
continuously listens through the built-in PDM microphone for a trigger phrase
("Lo-Bug"). Once the phrase is detected the device immediately begins recording
the user's spoken command, sends it to the local server (Whisper STT, Ollama
LLM, Piper TTS), and plays the spoken response back through the speaker.

The detection is implemented as a **two-phase plan**:

| Phase | Detector | Status |
|-------|----------|--------|
| **Phase 1** (current) | Amplitude-based -- sustained loud sound triggers detection. Acts as a functional stand-in while the real model is being trained. | Implemented |
| **Phase 2** (planned)  | Edge Impulse TFLite Micro -- a small neural network trained to recognize the specific phrase "Lo-Bug" running on-device inference. | Design documented below |

Both phases share the exact same pipeline: circular buffer, I2S management,
seamless mic handoff, and state machine transitions. Only the
`detect_wake_amplitude()` function needs to be swapped out.

---

## 2. Architecture

### 2.1 State Machine

The firmware's main loop is driven by a single `AppState` enum. Wake word
detection adds the `STATE_WAKE_LISTENING` state. The full flow is:

```
                          +------------------+
                          |   STATE_INIT     |
                          +--------+---------+
                                   |
                                   v
                     +-------------+-------------+
                     |    STATE_CONNECTING        |
                     |  WiFi + server discovery   |
                     +-------------+-------------+
                                   |
                                   v
              +--------------------+--------------------+
              |          STATE_WAKE_LISTENING            |
              |                                         |
              |  - Avatar shows Sleepy expression       |
              |  - wake_detect_feed() called each loop  |
              |  - Circular buffer continuously fills   |
              |  - Periodic blink animation plays       |
              |                                         |
              +---+----------+-----------+----------+---+
                  |          |           |          |
            wake  |    BtnA  |     BtnB  |    BtnC  |
           detect |  (Talk)  |   (Type)  |  (Menu)  |
                  |          |           |          |
                  v          v           |          v
           +------+----+  +-+--------+  |   show info
           | RECORDING  |  | RECORDING|  |   string
           | (from wake)|  | (manual) |  |
           +------+-----+  +----+----+  v
                  |              |    +--+--------+
                  +--------------+    | KEYBOARD  |
                         |            +-----+-----+
                         v                  |
                +--------+--------+         |
                | SENDING_AUDIO   |         |
                +--------+--------+         |
                         |                  |
                         v                  |
              +----------+----------+       |
              |  PLAYING_RESPONSE   |       |
              +----------+----------+       |
                         |                  |
                         +--------+---------+
                                  |
                                  v
                     (back to STATE_WAKE_LISTENING)
```

Key transitions related to wake detection:

- **CONNECTING -> WAKE_LISTENING**: After WiFi and server are found,
  `wake_detect_start()` is called and the device enters the listening state.
- **WAKE_LISTENING -> RECORDING (wake)**: When `wake_detect_feed()` returns
  `true`, `audio_capture_start_from_wake()` is called. The mic is NOT
  restarted -- it continues running seamlessly.
- **WAKE_LISTENING -> RECORDING (manual)**: When BtnA is pressed,
  `wake_detect_stop()` is called (tears down mic), then
  `audio_capture_start()` initializes the mic fresh.
- **PLAYING_RESPONSE -> WAKE_LISTENING**: After playback finishes (or is
  interrupted by a button), the speaker is stopped and
  `wake_detect_start()` restarts the mic for the next wake cycle.

### 2.2 I2S Bus Management

The M5Stack Core2 shares a **single I2S peripheral** between the built-in PDM
microphone and the NS4168 speaker amplifier. Only one can be active at a time.
The M5Unified library wraps this as `M5.Mic` and `M5.Speaker`.

The rule enforced throughout the codebase:

```
Speaker active  -->  Mic.end() then Speaker.begin()
Mic active      -->  Speaker.end() then Mic.begin()
```

Who owns the I2S bus at each state:

| State | I2S Owner | Notes |
|-------|-----------|-------|
| CONNECTING | Speaker | Default after M5.begin() |
| WAKE_LISTENING | **Mic** | `wake_detect_start()` calls `Speaker.end()` then `Mic.begin()` |
| RECORDING (from wake) | **Mic** | Mic stays running -- no I2S switch |
| RECORDING (manual) | **Mic** | `wake_detect_stop()` ends mic; `audio_capture_start()` restarts it |
| SENDING_AUDIO | Neither | `audio_capture_stop()` calls `Mic.end()` only. Speaker is started later by whoever needs it (playback or wake detect). |
| PLAYING_RESPONSE | Speaker | `audio_playback_play()` calls `Speaker.begin()` then `Speaker.playRaw()` |
| KEYBOARD | Speaker | Wake detector was stopped before entering |

### 2.3 Seamless Mic Handoff

When the wake word is detected, we do NOT want to stop the mic and restart it.
That would cause a ~50-100ms gap where audio is lost -- potentially clipping the
beginning of the user's command.

Instead:

1. `wake_detect_feed()` returns `true`.
2. The main loop calls `audio_capture_start_from_wake()`.
3. Inside that function:
   - `wake_detect_suspend()` is called, which sets `listening = false` but
     does **not** call `Mic.end()`. The I2S hardware keeps running.
   - The circular buffer contents are linearized and copied into the WAV
     recording buffer (starting at byte offset 44, after the WAV header).
   - `audio_capture_update()` then continues reading from `M5.Mic.record()`
     using the already-running mic.

This gives a **zero-gap** transition from wake detection to command recording.

Compare with manual talk (BtnA), where:
1. `wake_detect_stop()` is called -- this DOES call `Mic.end()` then
   `Speaker.begin()`.
2. `audio_capture_start()` then calls `Speaker.end()` and `Mic.begin()` fresh.

The manual path accepts the small I2S switch gap because the user presses a
button and then speaks, so there is natural latency.

### 2.4 Circular Buffer Design

The wake detector maintains a circular (ring) buffer in heap memory that
continuously records the most recent audio:

```
Buffer size:  WAKE_BUF_SAMPLES = 16000 samples = 1 second at 16 kHz
Sample type:  int16_t (signed 16-bit PCM)
Memory:       32,000 bytes, allocated via heap_caps_malloc(MALLOC_CAP_8BIT)

              circ_pos (write cursor)
                 |
                 v
  +---+---+---+---+---+---+---+---+---+---+
  | . | . | X | X | X | X | . | . | . | . |   (conceptual)
  +---+---+---+---+---+---+---+---+---+---+
    ^                           ^
    |                           |
  newest (wrapped)           oldest valid sample
                             (when buffer is full, oldest = circ_pos)
```

**Write path** (`wake_detect_feed()`):

- Reads 480 samples (30 ms) from the mic via `M5.Mic.record()`.
- Writes each sample into `circ_buf[circ_pos]`, advancing `circ_pos` with
  modulo wrap: `circ_pos = (circ_pos + 1) % WAKE_BUF_SAMPLES`.
- Tracks `circ_valid` (total valid samples written, capped at buffer length).

**Read path** (`audio_capture_start_from_wake()`):

- If `circ_valid >= WAKE_BUF_SAMPLES`, the buffer is full. The oldest sample
  is at `circ_pos` (since the write cursor just overwrote it).
- If `circ_valid < WAKE_BUF_SAMPLES`, the buffer is not full. The oldest
  sample is at index 0.
- The function walks the circular buffer linearly, copying into the flat WAV
  recording buffer with modular index arithmetic.

This design means the recording always includes up to 1 second of audio from
before the wake word was detected, which helps capture any speech that
immediately followed the trigger phrase.

---

## 3. Module Reference

### 3.1 wake_detect.h

All functions are declared in `firmware/wake_detect.h` and implemented in
`firmware/wake_detect.cpp`.

#### `void wake_detect_init()`

Allocates the circular buffer (`WAKE_BUF_SAMPLES * sizeof(int16_t)` bytes) on
the heap using `heap_caps_malloc()` with `MALLOC_CAP_8BIT`. Zeroes the buffer.
Call once during `setup()`.

Prints `[WAKE] Initialized, buffer=16000 samples (32000 bytes)` on success, or
`[WAKE] Failed to allocate circular buffer!` on failure.

#### `void wake_detect_start()`

Prepares the wake detector for a new listening session:
- Resets `circ_pos`, `circ_valid`, and `wake_frames_above` to 0.
- Sets `listening = true`.
- Stops the speaker (`M5.Speaker.end()`).
- Configures the mic (sample rate 16 kHz, magnification 24, noise filter 64)
  and starts it (`M5.Mic.begin()`).

Call when transitioning into `STATE_WAKE_LISTENING`.

#### `bool wake_detect_feed()`

The main per-loop function. Must be called repeatedly while in the wake
listening state. Returns `true` if the wake word is detected.

Behavior:
1. If in cooldown period (`millis() < wake_cooldown_until`), reads audio into
   the circular buffer to keep it flowing, but skips detection. Returns `false`.
2. Reads 480 samples (30 ms) from the mic into `wake_chunk[]`.
3. Writes those samples into the circular buffer.
4. Passes the chunk to the detector function (`detect_wake_amplitude()` in
   Phase 1).
5. If detection triggers, sets cooldown (`wake_cooldown_until = millis() + 2000`)
   and returns `true`.

#### `void wake_detect_stop()`

Full shutdown of the wake detector:
- Sets `listening = false`.
- Waits for any in-progress mic recording to complete.
- Calls `M5.Mic.end()` to release the I2S bus.
- Calls `M5.Speaker.begin()` to hand the bus back to the speaker.

Use when transitioning away from wake listening via a button press (manual talk
or keyboard), where the mic needs to be fully released.

#### `void wake_detect_suspend()`

Lightweight deactivation -- sets `listening = false` but does NOT touch the I2S
hardware. The mic keeps running.

Used exclusively by `audio_capture_start_from_wake()` to perform the seamless
handoff. After calling this, `audio_capture` takes ownership of the mic.

#### `bool wake_detect_is_listening()`

Returns the current value of the `listening` flag. Useful for debug or guard
checks.

#### `const int16_t* wake_detect_get_buffer()`

Returns a pointer to the raw circular buffer data. Used by `audio_capture` to
read buffered audio during the seamless handoff.

#### `size_t wake_detect_get_buffer_len()`

Returns `WAKE_BUF_SAMPLES` (the total capacity of the circular buffer in
samples). Used to perform modular arithmetic when linearizing the buffer.

#### `size_t wake_detect_get_buffer_pos()`

Returns `circ_pos`, the current write position in the circular buffer. This is
where the *next* sample will be written. When the buffer is full, this also
points to the *oldest* sample.

#### `size_t wake_detect_get_valid_samples()`

Returns `circ_valid`, the number of valid samples that have been written. This
value increases up to `WAKE_BUF_SAMPLES` and then stays there. Used to
determine whether the buffer has wrapped.

#### `static bool detect_wake_amplitude(const int16_t* samples, size_t count)` (internal)

Phase 1 detection function (not exposed in the header). Scans the given frame
for peak amplitude. If the peak exceeds `WAKE_AMP_THRESHOLD` (3000), increments
`wake_frames_above`. If `WAKE_FRAMES_NEEDED` (4) consecutive frames are above
threshold, returns `true`. Any frame below threshold resets the counter to 0.

This function is the target for replacement in Phase 2.

### 3.2 audio_capture -- `audio_capture_start_from_wake()`

Declared in `firmware/audio_capture.h`, implemented in `firmware/audio_capture.cpp`.

```c
void audio_capture_start_from_wake();
```

Initializes a recording session without restarting the mic hardware. Steps:

1. Resets PCM write offset to 0.
2. Sets `voice_detected = false` -- lets the VAD naturally detect the user's
   actual command speech after the wake word trigger.
3. Calls `wake_detect_suspend()` to take ownership of the mic.
4. Starts recording fresh from the current mic position. The circular buffer
   contents (pre-trigger audio) are intentionally NOT copied because they
   contain the wake trigger sound and ambient noise, not useful for STT.
   The user's actual command comes after the wake word.

The circular buffer infrastructure remains available for Phase 2 where a real
wake word model may benefit from the pre-trigger audio context.

The function logs: `[MIC] Recording started (from wake, fresh buffer)`.

### 3.3 display_ui -- `display_wake_listening_update()`

Declared in `firmware/display_ui.h`, implemented in `firmware/display_ui.cpp`.

```c
void display_wake_listening_update();
```

Provides a subtle "alive" animation while the device is in wake listening mode.
The avatar displays a `Sleepy` expression. Every 4 seconds
(`WAKE_BLINK_INTERVAL`), the avatar briefly switches to `Neutral` (eyes open)
with a small mouth twitch (`setMouthOpenRatio(0.1f)`). After 300 ms
(`WAKE_BLINK_DURATION`), it returns to `Sleepy` with mouth closed.

This gives the user visual feedback that the device is powered on and listening,
without being distracting.

---

## 4. Configuration

All tunable parameters live in `firmware/config.h` or as statics at the top of
`firmware/wake_detect.cpp`.

### Parameters in config.h

| Parameter | Default | Description |
|-----------|---------|-------------|
| `WAKE_BUF_SAMPLES` | `16000` | Circular buffer size in samples. At 16 kHz this is 1 second of audio. Increasing this captures more pre-trigger audio but costs more RAM (2 bytes per sample). |
| `WAKE_FRAME_SAMPLES` | `480` | Number of samples per detection frame. 480 samples at 16 kHz = 30 ms. This value was chosen to match WakeNet's expected frame size and is a good default for TFLite models too. |

### Parameters in wake_detect.cpp (Phase 1 only)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `WAKE_AMP_THRESHOLD` | `3000` | Peak amplitude that counts as "loud." Range is 0-32767 for 16-bit signed audio. Noise floor is typically ~400-600. Lower values = more sensitive (more false triggers). Higher values = less sensitive (may miss quiet wake words). |
| `WAKE_FRAMES_NEEDED` | `4` | Number of consecutive frames that must exceed the threshold before triggering. At 30 ms per frame, `4` means ~120 ms of sustained loud audio. Raising this reduces false triggers from transient sounds (claps, bumps) but requires speaking louder/longer. |
| `WAKE_GRACE_FRAMES` | `10` | Number of frames to skip detection after mic starts (~300 ms). Prevents false triggers from I2S bus switching transients when transitioning from speaker to mic. |
| `WAKE_COOLDOWN_MS` | `2000` | Milliseconds to ignore detection after a trigger. Prevents the same utterance from triggering twice. Also prevents the device's own speaker playback from immediately re-triggering (though the mic is off during playback, the cooldown provides an additional guard). |

### Tuning Guidance

**Too many false triggers (device activates on random noise):**
- Increase `WAKE_AMP_THRESHOLD` (try 3000-5000).
- Increase `WAKE_FRAMES_NEEDED` (try 4-6).
- These are Phase 1 concerns only. Phase 2's neural network model will be
  far more selective.

**Wake word is not detected (you have to shout):**
- Decrease `WAKE_AMP_THRESHOLD` (try 1000-1500).
- Check the mic magnification in `wake_detect_start()` (`mic_cfg.magnification`).
  The default is 24. Increasing to 32 or 48 boosts the signal.
- Decrease `WAKE_FRAMES_NEEDED` to 2.

**Double triggers (one utterance fires twice):**
- Increase `WAKE_COOLDOWN_MS` (try 3000).

**Want more pre-trigger audio in the recording:**
- Increase `WAKE_BUF_SAMPLES`. For example, `32000` gives 2 seconds. This
  costs 64 KB of RAM.

---

## 5. Phase 2 Guide -- Edge Impulse TFLite Micro

This section is a step-by-step playbook for replacing the amplitude detector
with a trained neural network that recognizes the phrase "Lo-Bug."

### 5.1 Recording Training Data

You need two categories of audio samples:

| Category | Label | Description | Target count |
|----------|-------|-------------|--------------|
| Wake word | `lo_bug` | Clear recordings of "Lo-Bug" spoken naturally, at varying distances, speeds, and volumes | 50-200 clips |
| Background / noise | `_noise` or `_unknown` | Silence, ambient noise, other speech, music, coughs, claps -- anything that is NOT the wake word | 100-400 clips |

**Recording tips:**
- Use the M5Stack itself to record samples (ensures the model trains on the
  same mic/ADC characteristics it will run on). You can write a small sketch
  that records 1-second WAV files to an SD card.
- Alternatively, record on a phone/laptop and convert to 16 kHz mono 16-bit
  WAV. Quality will be slightly different from the on-device mic.
- Each clip should be exactly 1 second long (matching `WAKE_BUF_SAMPLES`).
- Include variety: different speakers, different rooms, different distances
  from the mic (10 cm to 1 m).
- For background noise, include clips of the environment where the device
  will live (room tone, TV, typing, etc.).

### 5.2 Training on Edge Impulse

1. Create a free account at [edgeimpulse.com](https://edgeimpulse.com).
2. Create a new project. Select "Audio" as the data type.
3. Upload your WAV files via the Data Acquisition tab. Label them `lo_bug` and
   `noise`.
4. Design an Impulse:
   - **Input block**: Time series data, window size = 1000 ms, window increase
     = 500 ms, frequency = 16000 Hz.
   - **Processing block**: MFCC (Mel-Frequency Cepstral Coefficients). Use
     default parameters (typically 13 coefficients, frame length 40 ms,
     frame stride 20 ms).
   - **Learning block**: Classification (Keras). A small dense network or 1D
     CNN works well for wake words.
5. Generate features (look for clean cluster separation in the feature
   explorer).
6. Train the model. Target >95% accuracy on the validation set. Training
   usually takes under 5 minutes for a small wake word dataset.
7. Test with the "Model testing" tab to verify real-world accuracy.

### 5.3 Exporting the Model

1. Go to the **Deployment** tab.
2. Select **Arduino library** as the deployment target.
3. Select **Quantized (int8)** for the optimization -- this is critical for
   ESP32 performance. Float32 models are ~4x slower on ESP32.
4. Click **Build** and download the `.zip` file.
5. The zip contains a complete Arduino library with:
   - `model-parameters/model_metadata.h` -- input/output sizes
   - `tflite-model/trained_model_compiled.cpp` -- the quantized TFLite model
   - `edge-impulse-sdk/` -- the TFLite Micro runtime
   - `ei_classifier_*.h` -- high-level inference API

### 5.4 Integrating into wake_detect.cpp

The integration touches only `firmware/wake_detect.cpp`. No other files need
to change.

**Step 1: Add includes**

At the top of `wake_detect.cpp`, add:

```cpp
// Phase 2: Edge Impulse inference
#include <your-project-name_inferencing.h>

// Inference buffer (must match model's expected input length)
static float ei_features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
```

Replace `your-project-name` with the actual library name from the Edge Impulse
export.

**Step 2: Replace `detect_wake_amplitude()` with `detect_wake_ei()`**

Remove or `#ifdef` out the entire Phase 1 section (lines 16-51 in the current
file) and replace with:

```cpp
// --- Phase 2: Edge Impulse TFLite Micro wake detection ---

static bool detect_wake_ei() {
    // The model expects a 1-second window of audio.
    // Linearize the circular buffer into the feature array.
    size_t buf_len = WAKE_BUF_SAMPLES;
    size_t start = (circ_valid >= buf_len) ? circ_pos : 0;

    for (size_t i = 0; i < buf_len; i++) {
        size_t idx = (start + i) % buf_len;
        // Edge Impulse expects float features, normalized to [-1.0, 1.0]
        // or raw int16 depending on your DSP block config.
        // For raw audio input:
        ei_features[i] = (float)circ_buf[idx];
    }

    // Wrap in an Edge Impulse signal
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = [](size_t offset, size_t length, float *out) -> int {
        memcpy(out, ei_features + offset, length * sizeof(float));
        return 0;
    };

    // Run inference
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        Serial.printf("[WAKE] Inference error: %d\n", err);
        return false;
    }

    // Check classification result
    // Find the index of the "lo_bug" label
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (strcmp(result.classification[i].label, "lo_bug") == 0) {
            float confidence = result.classification[i].value;
            if (confidence > 0.8f) {  // 80% confidence threshold
                Serial.printf("[WAKE] 'Lo-Bug' detected (%.2f%%)\n",
                              confidence * 100.0f);
                return true;
            }
        }
    }

    return false;
}

// --- End Phase 2 detector ---
```

**Step 3: Update the call in `wake_detect_feed()`**

Change the detection call from:

```cpp
bool detected = detect_wake_amplitude(wake_chunk, WAKE_CHUNK_SAMPLES);
```

to:

```cpp
// Run inference every N frames (e.g., every 500ms = ~16 frames at 30ms each)
// to avoid running the model 33 times per second.
static int frame_counter = 0;
frame_counter++;
bool detected = false;
if (frame_counter >= 16) {  // ~480ms between inferences
    frame_counter = 0;
    detected = detect_wake_ei();
}
```

Running inference on every single 30 ms frame would be wasteful. The model
processes a full 1-second window, so running it every ~500 ms with 50% overlap
is sufficient and keeps CPU usage reasonable.

**Step 4: Tune the confidence threshold**

The `0.8f` (80%) threshold in the code above is a starting point. Adjust based
on testing:
- **Too many false triggers**: raise to 0.85 or 0.90.
- **Too many missed detections**: lower to 0.70 or 0.75.

### 5.5 Adding the Library to platformio.ini

Extract the downloaded Edge Impulse `.zip` into the `lib/` folder of the
project, then add it to `platformio.ini`:

```ini
lib_deps =
    m5stack/M5Unified@^0.1.6
    m5stack/M5GFX@^0.1.6
    bblanchon/ArduinoJson@^6.21.3
    meganetaaan/M5Stack-Avatar@^0.10.0
    ; Phase 2: Edge Impulse wake word model
    ; (extracted to lib/your-project-name/)
```

If you placed it in `lib/`, PlatformIO will auto-detect it. Alternatively, you
can reference it explicitly:

```ini
lib_extra_dirs = lib
```

You may also need to add build flags for TFLite Micro compatibility:

```ini
build_flags =
    -DBOARD_HAS_PSRAM
    -mfix-esp32-psram-cache-issue
    -D CONFIG_SPIRAM_CACHE_WORKAROUND
    -O2
    -DEI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
```

The `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN` flag enables the Espressif-optimized
neural network kernels, which significantly speed up inference on ESP32.

---

## 6. Why Not ESP-SR / WakeNet?

Espressif provides their own wake word engine called **ESP-SR** (which includes
**WakeNet** for wake word detection and **MultiNet** for command recognition).
It would seem like the natural choice for an ESP32 project. However:

**ESP-SR does not work with the Arduino framework on the original ESP32
(ESP32-D0WD / ESP32-WROOM).**

The reasons:

1. **Chip support**: ESP-SR's WakeNet v8/v9 models are compiled only for
   ESP32-S3 and ESP32-P4. These chips have vector instructions (ESP32-S3 has
   the PIE SIMD extension) that WakeNet's quantized models depend on. The
   original ESP32 lacks these instructions.

2. **Framework support**: Even the older WakeNet models (v5/v6) that did
   support the original ESP32 were only distributed as precompiled static
   libraries for the **ESP-IDF** build system. They rely on ESP-IDF components
   (`esp_partition`, `esp_flash`, etc.) that are not available or compatible
   when building under the Arduino framework.

3. **M5Stack Core2 uses the original ESP32**: The Core2 is based on the
   ESP32-D0WDQ5 (dual-core, no S3/P4 extensions). This rules out all current
   ESP-SR models.

**What this means for us:**

- Phase 1 uses simple amplitude detection as a working placeholder.
- Phase 2 uses **Edge Impulse + TensorFlow Lite Micro** (TFLite Micro), which
  is fully compatible with the original ESP32 under Arduino. TFLite Micro is a
  framework-agnostic C++ library with no ESP-IDF dependencies.
- If you ever migrate to an **M5Stack CoreS3** (which uses ESP32-S3), you could
  use ESP-SR/WakeNet natively via ESP-IDF -- but you would need to switch from
  Arduino to the ESP-IDF framework.

---

## 7. Troubleshooting

### False Triggers

**Symptom**: Device activates when nobody said the wake word.

| Cause | Fix |
|-------|-----|
| `WAKE_AMP_THRESHOLD` too low | Increase to 3000-5000 (Phase 1) |
| Environmental noise (fan, TV) | Increase `WAKE_FRAMES_NEEDED` to 5-6 |
| Vibrations/bumps registering as audio | Ensure device is on a stable surface; increase `WAKE_FRAMES_NEEDED` |
| Phase 2 model undertrained | Add more `_noise` samples and retrain; raise confidence threshold |

### No Triggers (Wake Word Not Detected)

**Symptom**: Speaking "Lo-Bug" clearly does not activate the device.

| Cause | Fix |
|-------|-----|
| `WAKE_AMP_THRESHOLD` too high | Lower to 1000-1500 (Phase 1) |
| Mic magnification too low | In `wake_detect_start()`, try `mic_cfg.magnification = 32` or `48` |
| Buffer allocation failed | Check serial output for `[WAKE] Failed to allocate circular buffer!` |
| Not in `STATE_WAKE_LISTENING` | Check serial output for `[APP] State:` transitions |
| Cooldown still active | Wait 2 seconds after last trigger before trying again |
| Phase 2 model confidence threshold too high | Lower from 0.8 to 0.7 |

### I2S Bus Hangs

**Symptom**: Device freezes or audio stops working after several wake/record
cycles.

| Cause | Fix |
|-------|-----|
| `Mic.end()` called without waiting for recording to finish | Both `wake_detect_stop()` and `audio_capture_stop()` include `while (M5.Mic.isRecording()) { delay(1); }` -- verify this is not bypassed |
| Speaker and Mic started simultaneously | Ensure every code path calls `Speaker.end()` before `Mic.begin()` and vice versa. Search for any path that violates this |
| M5Unified library bug | Update `M5Unified` to latest version in `platformio.ini` |

### Serial Debug Messages Reference

These are the key messages to look for in the serial monitor (115200 baud):

```
[WAKE] Initialized, buffer=16000 samples (32000 bytes)
    --> wake_detect_init() succeeded

[WAKE] Failed to allocate circular buffer!
    --> Out of memory. Reduce WAKE_BUF_SAMPLES or free other allocations.

[WAKE] Listening started
    --> wake_detect_start() completed, mic is active

[WAKE] >>> Wake word detected! <<<
    --> Detection triggered, transitioning to recording

[WAKE] Suspended (mic handed off)
    --> wake_detect_suspend() called, audio_capture owns the mic now

[WAKE] Listening stopped
    --> wake_detect_stop() completed, speaker is active

[MIC] Recording started (from wake, fresh buffer)
    --> audio_capture_start_from_wake() completed, recording from current mic position

[APP] State: 2 -> 3
    --> State transition (see AppState enum for numbers:
        0=INIT, 1=CONNECTING, 2=IDLE, 3=WAKE_LISTENING,
        4=RECORDING, 5=SENDING_AUDIO, 6=SENDING_TEXT,
        7=PLAYING_RESPONSE, 8=KEYBOARD)
```

### Audio Quality Issues

**Symptom**: Recordings are quiet, noisy, or distorted.

- **Quiet recordings**: Increase `mic_cfg.magnification` (default 24, max ~64).
  Note this is set in both `wake_detect_start()` and `audio_capture_start()` --
  keep them in sync.
- **Noisy recordings**: Increase `mic_cfg.noise_filter_level` (default 64,
  higher = more filtering). Be careful not to filter out actual speech.
- **Distorted/clipping**: Decrease `mic_cfg.magnification`. If peak samples
  are hitting +/-32767 regularly, the gain is too high.

---

## 8. Files Modified / Created

| File | Status | Summary |
|------|--------|---------|
| `firmware/wake_detect.h` | **Created** | Header declaring the wake detection API: init, start, stop, suspend, feed, and circular buffer accessors. |
| `firmware/wake_detect.cpp` | **Created** | Implementation of the wake detection module. Contains the circular buffer, I2S mic management, Phase 1 amplitude detector (`detect_wake_amplitude`), and cooldown logic. |
| `firmware/audio_capture.h` | **Modified** | Added declaration for `audio_capture_start_from_wake()`. |
| `firmware/audio_capture.cpp` | **Modified** | Added `#include "wake_detect.h"`. Implemented `audio_capture_start_from_wake()` which performs the seamless mic handoff by calling `wake_detect_suspend()` and linearizing the circular buffer into the WAV recording buffer. |
| `firmware/display_ui.h` | **Modified** | Added declaration for `display_wake_listening_update()`. |
| `firmware/display_ui.cpp` | **Modified** | Added wake listening animation state variables (`wake_blink_timer`, `WAKE_BLINK_INTERVAL`, `WAKE_BLINK_DURATION`). Implemented `display_wake_listening_update()` which produces a periodic blink on the Sleepy avatar face. |
| `firmware/config.h` | **Modified** | Added `WAKE_BUF_SAMPLES` (16000) and `WAKE_FRAME_SAMPLES` (480) parameters for wake word detection. |
| `firmware/firmware.ino` | **Modified** | Added `#include "wake_detect.h"`. Added `STATE_WAKE_LISTENING` to `AppState` enum. Added `wake_detect_init()` call in `setup()`. Added `handle_wake_listening()` state handler implementing wake-to-record flow. Modified `handle_connecting()` and all return-to-idle transitions to enter `STATE_WAKE_LISTENING` via `wake_detect_start()`. Added wake word detection path in `handle_wake_listening()` that calls `audio_capture_start_from_wake()` for seamless transition. |
| `firmware/audio_playback.cpp` | **Modified** | Added `M5.Speaker.begin()` call before `playRaw()` to ensure speaker is initialized after mic releases the I2S bus. |
| `platformio.ini` | **Unchanged** | No changes for Phase 1. Phase 2 will require adding the Edge Impulse library to `lib_deps` and potentially adding build flags (see Section 5.5). |
