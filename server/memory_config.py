"""Memory system configuration — all thresholds and tunables in one place."""

from dataclasses import dataclass


@dataclass
class MemoryConfig:
    """Tunables for the persistent memory system."""

    # Database
    db_path: str = "lobug_memory.db"

    # Short-term cascade: summarize after this many unsummarized messages
    short_term_threshold: int = 30
    short_term_keep: int = 10  # recent messages to keep unsummarized after cascade

    # Long-term cascade: distill base memory after this many unincorporated summaries
    long_term_threshold: int = 20
    long_term_keep: int = 10

    # Semantic retrieval
    retrieval_top_k: int = 5
    retrieval_min_similarity: float = 0.3  # cosine similarity floor

    # Embedding model
    embedding_model: str = "nomic-embed-text"
    embedding_dim: int = 768

    # Duplicate detection: cosine distance below which a new fact replaces the old
    duplicate_distance_threshold: float = 0.15
