"""Memory orchestration — retrieval, fact extraction, and cascade logic."""

import json
import logging
from dataclasses import dataclass, field
from typing import Callable

from memory_config import MemoryConfig
from memory_db import MemoryDB
from memory_prompts import (
    distill_base_memory_messages,
    fact_extraction_messages,
    summarize_conversation_messages,
)

logger = logging.getLogger(__name__)


@dataclass
class MemoryContext:
    """The memory payload injected into the system prompt."""
    base_memory: str
    relevant_facts: list[str]
    formatted: str  # ready-to-inject text block


class MemoryManager:
    """Orchestrates retrieval, extraction, and cascade operations.

    Accepts embed_fn and chat_fn as callables so it is fully LLM-backend-agnostic.
    """

    def __init__(
        self,
        db: MemoryDB,
        embed_fn: Callable[[str], list[float]],
        chat_fn: Callable[[list[dict]], str],
        config: MemoryConfig | None = None,
    ):
        self.db = db
        self.embed_fn = embed_fn
        self.chat_fn = chat_fn
        self.config = config or db.config

    # ------------------------------------------------------------------
    # Retrieval (synchronous, called before LLM response)
    # ------------------------------------------------------------------

    def retrieve_context(self, query: str) -> MemoryContext:
        """Embed the query, KNN-search facts, fetch base memory, return formatted context."""
        base_memory = self.db.get_base_memory()
        relevant_facts: list[str] = []

        try:
            query_vec = self.embed_fn(query)
            results = self.db.search_facts(query_vec, k=self.config.retrieval_top_k)
            # cosine distance: 0 = identical, 2 = opposite
            # similarity = 1 - distance; keep facts above min_similarity
            max_distance = 1.0 - self.config.retrieval_min_similarity
            relevant_facts = [
                r.content for r in results if r.distance <= max_distance
            ]
        except Exception as e:
            logger.warning("Memory retrieval failed (non-fatal): %s", e)

        formatted = self._format_context(base_memory, relevant_facts)
        return MemoryContext(
            base_memory=base_memory,
            relevant_facts=relevant_facts,
            formatted=formatted,
        )

    @staticmethod
    def _format_context(base_memory: str, facts: list[str]) -> str:
        """Build the text block that gets injected into the system prompt."""
        parts: list[str] = []
        if base_memory:
            parts.append(f"[Long-term memory]\n{base_memory}")
        if facts:
            bullet_list = "\n".join(f"- {f}" for f in facts)
            parts.append(f"[Relevant memories]\n{bullet_list}")
        return "\n\n".join(parts)

    # ------------------------------------------------------------------
    # Background processing (called after response is sent)
    # ------------------------------------------------------------------

    def process_exchange(self, user_text: str, assistant_text: str) -> None:
        """Extract facts and run cascade checks. Safe to call in a background task."""
        try:
            self._extract_facts(user_text, assistant_text)
        except Exception as e:
            logger.error("Fact extraction failed (non-fatal): %s", e)

        try:
            self._check_short_term_cascade()
        except Exception as e:
            logger.error("Short-term cascade failed (non-fatal): %s", e)

        try:
            self._check_long_term_cascade()
        except Exception as e:
            logger.error("Long-term cascade failed (non-fatal): %s", e)

    # --- Fact extraction ---

    def _extract_facts(self, user_text: str, assistant_text: str) -> None:
        messages = fact_extraction_messages(user_text, assistant_text)
        raw = self.chat_fn(messages)

        facts = self._parse_facts_json(raw)
        if not facts:
            return

        for item in facts:
            fact_text = item.get("fact", "").strip()
            fact_type = item.get("type", "knowledge")
            if not fact_text:
                continue

            vec = self.embed_fn(fact_text)
            self._deduplicate_and_insert(fact_text, vec, fact_type)

        logger.info("Extracted %d facts from exchange", len(facts))

    @staticmethod
    def _parse_facts_json(raw: str) -> list[dict]:
        """Best-effort parse of the LLM's JSON array output."""
        text = raw.strip()
        # Strip markdown fences if the LLM wrapped output
        if text.startswith("```"):
            lines = text.splitlines()
            lines = [l for l in lines if not l.startswith("```")]
            text = "\n".join(lines)
        try:
            parsed = json.loads(text)
            if isinstance(parsed, list):
                return parsed
        except json.JSONDecodeError:
            logger.warning("Could not parse fact-extraction JSON: %s", text[:200])
        return []

    def _deduplicate_and_insert(
        self, fact_text: str, vec: list[float], fact_type: str
    ) -> None:
        """Check for near-duplicate facts; if found, delete old and insert new."""
        existing = self.db.search_facts(vec, k=1)
        if existing and existing[0].distance < self.config.duplicate_distance_threshold:
            logger.debug(
                "Replacing duplicate fact (dist=%.3f): '%s' → '%s'",
                existing[0].distance, existing[0].content, fact_text,
            )
            self.db.delete_fact(existing[0].id)

        self.db.insert_fact(fact_text, vec, source="extraction", fact_type=fact_type)

    # --- Short-term cascade (conversation → summary) ---

    def _check_short_term_cascade(self) -> None:
        count = self.db.count_unsummarized()
        if count < self.config.short_term_threshold:
            return

        logger.info("Short-term cascade triggered (%d unsummarized messages)", count)
        msgs = self.db.get_unsummarized_messages()

        # Keep the most recent `short_term_keep` unsummarized
        to_summarize = msgs[: len(msgs) - self.config.short_term_keep]
        if not to_summarize:
            return

        # Format conversation block
        conv_text = "\n".join(
            f"{m.role.capitalize()}: {m.content}" for m in to_summarize
        )

        summary_messages = summarize_conversation_messages(conv_text)
        summary = self.chat_fn(summary_messages).strip()

        self.db.insert_summary(
            content=summary,
            source_from_id=to_summarize[0].id,
            source_to_id=to_summarize[-1].id,
        )
        self.db.mark_summarized(to_summarize[-1].id)
        logger.info(
            "Summarized messages %d–%d", to_summarize[0].id, to_summarize[-1].id
        )

    # --- Long-term cascade (summaries → base memory) ---

    def _check_long_term_cascade(self) -> None:
        count = self.db.count_unincorporated_summaries()
        if count < self.config.long_term_threshold:
            return

        logger.info("Long-term cascade triggered (%d unincorporated summaries)", count)
        summaries = self.db.get_unincorporated_summaries()

        # Keep the most recent `long_term_keep` unincorporated
        to_incorporate = summaries[: len(summaries) - self.config.long_term_keep]
        if not to_incorporate:
            return

        current_base = self.db.get_base_memory()
        summaries_text = "\n\n".join(s.content for s in to_incorporate)

        # Gather recent facts for context
        recent_facts_text = ""
        try:
            # Use a generic query to pull some recent facts
            vec = self.embed_fn("user information and preferences")
            facts = self.db.search_facts(vec, k=10)
            if facts:
                recent_facts_text = "\n".join(f"- {f.content}" for f in facts)
        except Exception:
            pass

        distill_messages = distill_base_memory_messages(
            current_base, summaries_text, recent_facts_text
        )
        new_base = self.chat_fn(distill_messages).strip()

        self.db.set_base_memory(new_base)
        self.db.mark_incorporated(to_incorporate[-1].id)
        logger.info("Base memory updated, incorporated %d summaries", len(to_incorporate))
