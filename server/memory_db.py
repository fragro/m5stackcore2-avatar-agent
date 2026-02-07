"""SQLite + sqlite-vec database layer for persistent memory."""

import logging
import struct
import time
from dataclasses import dataclass

try:
    import pysqlite3 as sqlite3  # has enable_load_extension support
except ImportError:
    import sqlite3

from memory_config import MemoryConfig

logger = logging.getLogger(__name__)


@dataclass
class FactRow:
    """A fact retrieved from the vector store."""
    id: int
    content: str
    source: str
    fact_type: str
    created_at: float
    distance: float  # cosine distance (0 = identical, 2 = opposite)


@dataclass
class MessageRow:
    """A row from conversation_log."""
    id: int
    role: str
    content: str
    created_at: float


@dataclass
class SummaryRow:
    """A row from memory_summaries."""
    id: int
    content: str
    source_from_id: int
    source_to_id: int
    created_at: float


def serialize_vector(vec: list[float]) -> bytes:
    """Pack a float list into bytes for sqlite-vec."""
    return struct.pack(f"{len(vec)}f", *vec)


class MemoryDB:
    """All database operations for the memory system."""

    def __init__(self, config: MemoryConfig | None = None):
        self.config = config or MemoryConfig()
        self.conn: sqlite3.Connection | None = None

    def initialize(self) -> None:
        """Open the database, load sqlite-vec, create tables, enable WAL."""
        self.conn = sqlite3.connect(self.config.db_path, check_same_thread=False)
        self.conn.execute("PRAGMA journal_mode=WAL")

        # Load sqlite-vec extension
        import sqlite_vec
        self.conn.enable_load_extension(True)
        sqlite_vec.load(self.conn)
        self.conn.enable_load_extension(False)

        self._create_tables()
        logger.info("Memory DB initialized at %s", self.config.db_path)

    def _create_tables(self) -> None:
        cur = self.conn.cursor()

        # Conversation log — all turns, survives restarts
        cur.execute("""
            CREATE TABLE IF NOT EXISTS conversation_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                created_at REAL NOT NULL,
                summarized INTEGER NOT NULL DEFAULT 0
            )
        """)

        # Facts with vector embeddings (sqlite-vec virtual table)
        dim = self.config.embedding_dim
        cur.execute(f"""
            CREATE VIRTUAL TABLE IF NOT EXISTS memory_facts USING vec0 (
                id INTEGER PRIMARY KEY,
                embedding float[{dim}] distance_metric=cosine,
                +content TEXT,
                +source TEXT,
                +fact_type TEXT,
                +created_at FLOAT
            )
        """)

        # Conversation summaries
        cur.execute("""
            CREATE TABLE IF NOT EXISTS memory_summaries (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                content TEXT NOT NULL,
                source_from_id INTEGER NOT NULL,
                source_to_id INTEGER NOT NULL,
                created_at REAL NOT NULL,
                incorporated INTEGER NOT NULL DEFAULT 0
            )
        """)

        # Base memory — single row
        cur.execute("""
            CREATE TABLE IF NOT EXISTS memory_base (
                id INTEGER PRIMARY KEY CHECK(id = 1),
                content TEXT NOT NULL DEFAULT '',
                updated_at REAL NOT NULL
            )
        """)

        # Ensure the single base-memory row exists
        cur.execute("""
            INSERT OR IGNORE INTO memory_base (id, content, updated_at)
            VALUES (1, '', ?)
        """, (time.time(),))

        self.conn.commit()

    # --- Conversation log ---

    def append_message(self, role: str, content: str) -> int:
        """Insert a message into conversation_log. Returns its row id."""
        cur = self.conn.execute(
            "INSERT INTO conversation_log (role, content, created_at) VALUES (?, ?, ?)",
            (role, content, time.time()),
        )
        self.conn.commit()
        return cur.lastrowid

    def get_recent_messages(self, n: int) -> list[MessageRow]:
        """Return the last n messages (oldest first)."""
        rows = self.conn.execute(
            "SELECT id, role, content, created_at FROM conversation_log "
            "ORDER BY id DESC LIMIT ?",
            (n,),
        ).fetchall()
        return [MessageRow(*r) for r in reversed(rows)]

    def count_unsummarized(self) -> int:
        row = self.conn.execute(
            "SELECT COUNT(*) FROM conversation_log WHERE summarized = 0"
        ).fetchone()
        return row[0]

    def get_unsummarized_messages(self) -> list[MessageRow]:
        """Return all unsummarized messages, oldest first."""
        rows = self.conn.execute(
            "SELECT id, role, content, created_at FROM conversation_log "
            "WHERE summarized = 0 ORDER BY id ASC"
        ).fetchall()
        return [MessageRow(*r) for r in rows]

    def mark_summarized(self, up_to_id: int) -> None:
        """Mark all messages up to (inclusive) the given id as summarized."""
        self.conn.execute(
            "UPDATE conversation_log SET summarized = 1 WHERE id <= ?",
            (up_to_id,),
        )
        self.conn.commit()

    # --- Facts (vector store) ---

    def insert_fact(
        self, content: str, embedding: list[float], source: str, fact_type: str
    ) -> int:
        """Insert a fact with its embedding vector. Returns the row id."""
        vec_bytes = serialize_vector(embedding)
        cur = self.conn.execute(
            "INSERT INTO memory_facts (embedding, content, source, fact_type, created_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (vec_bytes, content, source, fact_type, time.time()),
        )
        self.conn.commit()
        return cur.lastrowid

    def search_facts(self, query_embedding: list[float], k: int) -> list[FactRow]:
        """KNN search over memory_facts. Returns up to k results sorted by distance."""
        vec_bytes = serialize_vector(query_embedding)
        rows = self.conn.execute(
            "SELECT id, distance, content, source, fact_type, created_at "
            "FROM memory_facts WHERE embedding MATCH ? AND k = ?",
            (vec_bytes, k),
        ).fetchall()
        return [
            FactRow(
                id=r[0], distance=r[1], content=r[2],
                source=r[3], fact_type=r[4], created_at=r[5],
            )
            for r in rows
        ]

    def delete_fact(self, fact_id: int) -> None:
        """Delete a fact by id."""
        self.conn.execute("DELETE FROM memory_facts WHERE id = ?", (fact_id,))
        self.conn.commit()

    # --- Summaries ---

    def insert_summary(
        self, content: str, source_from_id: int, source_to_id: int
    ) -> int:
        cur = self.conn.execute(
            "INSERT INTO memory_summaries (content, source_from_id, source_to_id, created_at) "
            "VALUES (?, ?, ?, ?)",
            (content, source_from_id, source_to_id, time.time()),
        )
        self.conn.commit()
        return cur.lastrowid

    def count_unincorporated_summaries(self) -> int:
        row = self.conn.execute(
            "SELECT COUNT(*) FROM memory_summaries WHERE incorporated = 0"
        ).fetchone()
        return row[0]

    def get_unincorporated_summaries(self) -> list[SummaryRow]:
        rows = self.conn.execute(
            "SELECT id, content, source_from_id, source_to_id, created_at "
            "FROM memory_summaries WHERE incorporated = 0 ORDER BY id ASC"
        ).fetchall()
        return [SummaryRow(*r) for r in rows]

    def mark_incorporated(self, up_to_id: int) -> None:
        self.conn.execute(
            "UPDATE memory_summaries SET incorporated = 1 WHERE id <= ?",
            (up_to_id,),
        )
        self.conn.commit()

    # --- Base memory ---

    def get_base_memory(self) -> str:
        row = self.conn.execute(
            "SELECT content FROM memory_base WHERE id = 1"
        ).fetchone()
        return row[0] if row else ""

    def set_base_memory(self, content: str) -> None:
        self.conn.execute(
            "UPDATE memory_base SET content = ?, updated_at = ? WHERE id = 1",
            (content, time.time()),
        )
        self.conn.commit()
