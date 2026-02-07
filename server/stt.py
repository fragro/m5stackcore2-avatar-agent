"""Speech-to-text using faster-whisper."""

import io
import logging
import numpy as np
from faster_whisper import WhisperModel

logger = logging.getLogger(__name__)

_model: WhisperModel | None = None


def load_model(model_size: str = "base") -> None:
    """Load the Whisper model. Call once at startup."""
    global _model
    logger.info("Loading Whisper model: %s", model_size)
    _model = WhisperModel(model_size, device="cpu", compute_type="int8")
    logger.info("Whisper model loaded")


def transcribe(audio_bytes: bytes) -> str:
    """Transcribe WAV audio bytes to text.

    Args:
        audio_bytes: Raw WAV file bytes (16kHz, 16-bit, mono PCM).

    Returns:
        Transcribed text string.
    """
    if _model is None:
        raise RuntimeError("Whisper model not loaded. Call load_model() first.")

    # Parse WAV: skip 44-byte header, read 16-bit PCM samples
    if len(audio_bytes) < 44:
        return ""

    pcm_data = audio_bytes[44:]
    if len(pcm_data) == 0:
        return ""

    samples = np.frombuffer(pcm_data, dtype=np.int16).astype(np.float32) / 32768.0

    segments, info = _model.transcribe(samples, beam_size=5, language="en")
    text = " ".join(segment.text.strip() for segment in segments).strip()

    logger.info("Transcribed: '%s' (%.1fs audio, lang=%s)", text, info.duration, info.language)
    return text
