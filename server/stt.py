"""Speech-to-text using faster-whisper."""

import logging
from dataclasses import dataclass

import numpy as np
from faster_whisper import WhisperModel

logger = logging.getLogger(__name__)

_model: WhisperModel | None = None


@dataclass
class TranscriptionResult:
    text: str
    no_speech_prob: float       # max across all segments (0-1, higher = more likely noise)
    avg_no_speech_prob: float   # average across segments
    audio_duration: float       # seconds
    segment_count: int


def load_model(model_size: str = "base") -> None:
    """Load the Whisper model. Call once at startup."""
    global _model
    logger.info("Loading Whisper model: %s", model_size)
    _model = WhisperModel(model_size, device="cpu", compute_type="int8")
    logger.info("Whisper model loaded")


def transcribe(audio_bytes: bytes) -> TranscriptionResult:
    """Transcribe WAV audio bytes to text.

    Args:
        audio_bytes: Raw WAV file bytes (16kHz, 16-bit, mono PCM).

    Returns:
        TranscriptionResult with text, confidence metrics, and audio metadata.
    """
    empty = TranscriptionResult(text="", no_speech_prob=1.0, avg_no_speech_prob=1.0,
                                audio_duration=0.0, segment_count=0)

    if _model is None:
        raise RuntimeError("Whisper model not loaded. Call load_model() first.")

    # Parse WAV: skip 44-byte header, read 16-bit PCM samples
    if len(audio_bytes) < 44:
        return empty

    pcm_data = audio_bytes[44:]
    if len(pcm_data) == 0:
        return empty

    samples = np.frombuffer(pcm_data, dtype=np.int16).astype(np.float32) / 32768.0

    segments_gen, info = _model.transcribe(samples, beam_size=5, language="en")

    # Collect segments in one pass
    segments = list(segments_gen)

    text = " ".join(seg.text.strip() for seg in segments).strip()
    segment_count = len(segments)

    if segment_count > 0:
        no_speech_prob = max(seg.no_speech_prob for seg in segments)
        avg_no_speech_prob = sum(seg.no_speech_prob for seg in segments) / segment_count
    else:
        no_speech_prob = 1.0
        avg_no_speech_prob = 1.0

    result = TranscriptionResult(
        text=text,
        no_speech_prob=no_speech_prob,
        avg_no_speech_prob=avg_no_speech_prob,
        audio_duration=info.duration,
        segment_count=segment_count,
    )

    logger.info("Transcribed: '%s' (%.1fs, %d segs, no_speech=%.2f/%.2f)",
                text, info.duration, segment_count, no_speech_prob, avg_no_speech_prob)
    return result
