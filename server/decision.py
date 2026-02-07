"""Response decision logic: heuristic filtering and LLM response parsing."""

import logging
import re
from enum import Enum

from stt import TranscriptionResult

logger = logging.getLogger(__name__)


class Action(Enum):
    RESPOND = "respond"
    REACT = "react"
    IGNORE = "ignore"


# Exact-match hallucination blocklist (normalized: lowercase, stripped punctuation)
HALLUCINATION_BLOCKLIST = frozenset({
    "thank you",
    "thanks for watching",
    "please subscribe",
    "bye",
    "goodbye",
    "you",
    "the end",
    "see you next time",
    "so",
    "okay",
    "hmm",
    "uh",
    "um",
    "oh",
    "ah",
    "thanks",
    "subscribe",
    "youre welcome",
    "thank you for watching",
    "like and subscribe",
    "see you",
    "bye bye",
    "hey",
    "hi",
    "yeah",
    "yes",
    "no",
    "huh",
})

_PUNCT_RE = re.compile(r"[^\w\s]", re.UNICODE)


def _normalize(text: str) -> str:
    """Lowercase and strip punctuation for blocklist comparison."""
    return _PUNCT_RE.sub("", text.lower()).strip()


def _word_count(text: str) -> int:
    """Count word tokens after stripping punctuation."""
    cleaned = _PUNCT_RE.sub("", text).strip()
    if not cleaned:
        return 0
    return len(cleaned.split())


def heuristic_filter(result: TranscriptionResult) -> str | None:
    """Check if a transcription should be rejected before hitting the LLM.

    Returns a rejection reason string, or None if the transcription passes all checks.
    """
    # Too short audio — transient noise burst
    if result.audio_duration < 0.5:
        logger.info("Filtered: audio too short (%.2fs)", result.audio_duration)
        return "audio_too_short"

    # Whisper very uncertain about speech
    if result.avg_no_speech_prob > 0.7:
        logger.info("Filtered: high avg no_speech_prob (%.2f)", result.avg_no_speech_prob)
        return "high_no_speech_prob"

    # Too few words
    if _word_count(result.text) < 2:
        logger.info("Filtered: too few words ('%s')", result.text)
        return "too_few_words"

    # Hallucination blocklist
    normalized = _normalize(result.text)
    if normalized in HALLUCINATION_BLOCKLIST:
        logger.info("Filtered: hallucination blocklist match ('%s' -> '%s')", result.text, normalized)
        return "hallucination_blocklist"

    # Combined weak signals
    if (result.audio_duration < 1.0
            and result.no_speech_prob > 0.5
            and result.segment_count <= 1):
        logger.info("Filtered: combined weak signals (dur=%.2f, nsp=%.2f, segs=%d)",
                     result.audio_duration, result.no_speech_prob, result.segment_count)
        return "combined_weak_signals"

    return None


def parse_llm_response(raw: str) -> tuple[Action, str]:
    """Parse action prefix from LLM response text.

    Returns (action, cleaned_text) where cleaned_text has the prefix removed.
    """
    stripped = raw.strip()

    if stripped.startswith("[IGNORE]"):
        cleaned = stripped[len("[IGNORE]"):].strip()
        logger.info("LLM chose IGNORE: '%s'", cleaned)
        return Action.IGNORE, cleaned

    if stripped.startswith("[REACT]"):
        cleaned = stripped[len("[REACT]"):].strip()
        logger.info("LLM chose REACT: '%s'", cleaned)
        return Action.REACT, cleaned

    return Action.RESPOND, stripped
