"""Text-to-speech using macOS 'say' command as primary backend."""

from __future__ import annotations

import io
import wave
import logging
import subprocess
import shutil
import tempfile
from pathlib import Path

logger = logging.getLogger(__name__)

_backend: str = "say"


def load_model(voice: str | None = None) -> None:
    """Configure TTS backend. Uses macOS say."""
    global _backend

    if shutil.which("say"):
        _backend = "say"
        logger.info("TTS backend: macOS say")
    else:
        _backend = "none"
        logger.warning("No TTS backend available — 'say' command not found")


def synthesize(text: str, sample_rate: int = 16000) -> bytes:
    """Convert text to WAV audio bytes."""
    if not text.strip():
        return _empty_wav(sample_rate)

    if _backend == "say":
        return _synthesize_say(text, sample_rate)
    else:
        return _empty_wav(sample_rate)


def _synthesize_say(text: str, sample_rate: int) -> bytes:
    """Use macOS 'say' command for TTS."""
    try:
        with tempfile.NamedTemporaryFile(suffix=".aiff", delete=False) as tmp:
            tmp_path = tmp.name

        # Generate speech with 'say' -> AIFF file
        subprocess.run(
            ["say", "-o", tmp_path, text],
            capture_output=True,
            timeout=30,
        )

        # Convert AIFF to WAV using afconvert
        wav_path = tmp_path + ".wav"
        subprocess.run(
            ["afconvert", "-f", "WAVE", "-d", f"LEI16@{sample_rate}",
             tmp_path, wav_path],
            capture_output=True,
            timeout=10,
        )

        wav_bytes = Path(wav_path).read_bytes()

        # Clean up temp files
        Path(tmp_path).unlink(missing_ok=True)
        Path(wav_path).unlink(missing_ok=True)

        if len(wav_bytes) > 44:
            logger.info("TTS (say): generated %d bytes", len(wav_bytes))
            return wav_bytes

        return _empty_wav(sample_rate)

    except Exception as e:
        logger.error("macOS say error: %s", e)
        return _empty_wav(sample_rate)



def _raw_to_wav(raw_pcm: bytes, sample_rate: int) -> bytes:
    """Wrap raw 16-bit mono PCM in a WAV header."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(raw_pcm)
    return buf.getvalue()


def _empty_wav(sample_rate: int) -> bytes:
    """Return a minimal valid WAV file with silence."""
    n_samples = sample_rate // 10
    silence = b"\x00\x00" * n_samples
    return _raw_to_wav(silence, sample_rate)
