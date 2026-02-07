# Persistent Memory System -- Design & Implementation

Lo-Bug originally had the memory of a bug -- a 20-message `deque` that evicted
old messages and lost everything on server restart. This document covers the
tiered persistent memory system that gives Lo-Bug long-term recall, semantic
retrieval, and a growing understanding of its user.

Inspired by [LLM_Memory_Manager](https://github.com/Poppyqueen/LLM_Memory_Manager),
backed by sqlite-vec for semantic search, and abstracted from Ollama so the
embedding backend can be swapped to vLLM later.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
   - [Three-Tier Memory Model](#21-three-tier-memory-model)
   - [Per-Turn Data Flow](#22-per-turn-data-flow)
   - [System Prompt Structure](#23-system-prompt-structure)
3. [New Dependencies](#3-new-dependencies)
4. [File Reference](#4-file-reference)
   - [memory_config.py](#41-memory_configpy)
   - [memory_prompts.py](#42-memory_promptspy)
   - [memory_db.py](#43-memory_dbpy)
   - [memory_manager.py](#44-memory_managerpy)
5. [Modified Files](#5-modified-files)
   - [llm.py](#51-llmpy)
   - [conversation.py](#52-conversationpy)
   - [server.py](#53-serverpy)
   - [requirements.txt](#54-requirementstxt)
6. [Configuration](#6-configuration)
7. [Cascade Mechanics](#7-cascade-mechanics)
   - [Short-Term Cascade](#71-short-term-cascade)
   - [Long-Term Cascade](#72-long-term-cascade)
8. [Semantic Retrieval](#8-semantic-retrieval)
9. [Fact Deduplication](#9-fact-deduplication)
10. [Setup](#10-setup)
11. [Verification](#11-verification)
12. [Design Decisions](#12-design-decisions)

---

## 1. Overview

The memory system operates entirely server-side. No firmware changes are
required -- the HTTP API contract (`/chat/text`, `/chat/audio`,
`/context/sensors`, `/health`) is unchanged. The M5Stack device continues to
send and receive the same request/response payloads.

What changes is what happens inside the server between receiving a user message
and returning an LLM response:

- **Before**: 20-message rolling deque, no persistence, no retrieval.
- **After**: Messages are persisted to SQLite, facts are extracted and embedded,
  relevant memories are injected into the system prompt via KNN search, and
  periodic cascades summarize and distill long-term knowledge.

The key upgrade over plain LLM_Memory_Manager is **semantic retrieval**: instead
of injecting all stored memories into the prompt, we embed them as 768-dimensional
vectors and perform KNN cosine search. Only the top-K most relevant memories
enter the prompt for each turn.

---

## 2. Architecture

### 2.1 Three-Tier Memory Model

```
Working Memory (deque, 20 msgs)    <-- what the LLM sees as conversation turns
        | persisted to DB
        v
Conversation Log (SQLite)          <-- survives restarts, feeds summarization
        | every 30 unsummarized msgs
        v
Long-term: Facts + Summaries       <-- semantic search via sqlite-vec embeddings
        | every 20 unincorporated summaries
        v
Base Memory (single doc)           <-- always in system prompt, core identity
```

| Tier | Storage | Lifetime | Access Pattern |
|------|---------|----------|----------------|
| Working memory | `deque(maxlen=20)` | Session (but reloaded from DB on restart) | Full context window |
| Conversation log | `conversation_log` table | Permanent | Sequential scan for summarization |
| Facts | `memory_facts` vec0 table | Permanent | KNN vector search per turn |
| Summaries | `memory_summaries` table | Permanent | Batch read during cascade |
| Base memory | `memory_base` table (single row) | Permanent, overwritten on distill | Always injected into system prompt |

### 2.2 Per-Turn Data Flow

```
User: "My name is Alex and I work at NASA"
  |
  v
1. Persist to conversation_log
2. Add to deque (working memory)
3. build_messages():
   a. embed("My name is Alex...") --> 768-dim vector  (~50ms)
   b. KNN search memory_facts --> [relevant facts]     (<1ms)
   c. get_base_memory() --> base memory text
   d. Inject memory context into system prompt
4. llm.chat() --> "Nice to know, Alex. What can I help with?"
5. Persist response to conversation_log
6. Add response to deque
7. Return response to client immediately
  |
  v  (background, after HTTP response sent)
8. Extract facts via LLM:
   --> [{"fact": "The user's name is Alex", "type": "personal"},
       {"fact": "The user works at NASA", "type": "personal"}]
9. Embed each fact, check for near-duplicates, insert into memory_facts
10. Check cascade thresholds (no-op if below)
```

### 2.3 System Prompt Structure

After memory integration, the system prompt sent to the LLM is structured as:

```
[Lo-Bug personality + agency instructions]

[Long-term memory]
User Profile:
- Name: Alex, works at NASA
Preferences:
- Prefers concise answers
Key History:
- Building M5Stack voice assistant "Lo-Bug"

[Relevant memories]
- The user has a meeting with propulsion team Friday
- The user prefers Python for hardware projects

[Current physical state: Device orientation: face_up]

--- conversation history messages follow ---
```

The memory sections appear between the personality prompt and the sensor
context. If no memories exist yet, these sections are omitted entirely.

---

## 3. New Dependencies

| Package | Version | Purpose | Size |
|---------|---------|---------|------|
| `sqlite-vec` | >= 0.1.1 | Vector search extension for SQLite (vec0 virtual tables, KNN) | ~165 KB |
| `pysqlite3` | >= 0.5.4 | SQLite3 driver with `enable_load_extension` support (macOS stock Python lacks this) | ~950 KB |
| `nomic-embed-text` | (Ollama model) | 768-dimensional text embedding model | 274 MB |

`sqlite-vec` has zero native dependencies. `pysqlite3` bundles its own SQLite
build. The Ollama embedding model is pulled separately via `ollama pull`.

---

## 4. File Reference

### 4.1 `memory_config.py`

**New file.** A single `MemoryConfig` dataclass containing every tunable
parameter for the memory system. No logic, no imports beyond `dataclasses`.

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `db_path` | `"lobug_memory.db"` | SQLite database file path |
| `short_term_threshold` | `30` | Unsummarized message count before short-term cascade fires |
| `short_term_keep` | `10` | Recent messages to leave unsummarized after cascade |
| `long_term_threshold` | `20` | Unincorporated summary count before long-term cascade fires |
| `long_term_keep` | `10` | Recent summaries to leave unincorporated after cascade |
| `retrieval_top_k` | `5` | Number of facts returned by KNN search |
| `retrieval_min_similarity` | `0.3` | Cosine similarity floor (facts below this are filtered out) |
| `embedding_model` | `"nomic-embed-text"` | Ollama model name for embeddings |
| `embedding_dim` | `768` | Embedding vector dimensionality |
| `duplicate_distance_threshold` | `0.15` | Cosine distance below which a new fact replaces an existing one |

### 4.2 `memory_prompts.py`

**New file.** Three prompt-building functions, each returning `list[dict]` ready
for `llm.chat()`. No state, no side effects.

**`fact_extraction_messages(user_text, assistant_text)`**

Instructs the LLM to output a JSON array of `{"fact": "...", "type": "..."}`.
Rules enforced in the system prompt:
- Third-person standalone sentences
- Selective: skip chitchat, greetings, filler
- Valid types: `personal`, `preference`, `knowledge`, `event`
- Output only JSON, no commentary

**`summarize_conversation_messages(conversation_text)`**

Condenses a batch of conversation turns into a 3-5 sentence paragraph.
Third person, past tense, preserves specifics (names, dates, numbers).

**`distill_base_memory_messages(current_base, new_summaries, recent_facts)`**

Integrates new summaries and facts into the base memory document. Organized
into sections: User Profile, Preferences, Ongoing Topics, Key History. Max
~400 words.

### 4.3 `memory_db.py`

**New file.** The `MemoryDB` class encapsulates all SQLite + sqlite-vec
operations. Opens the database, loads the vec0 extension via pysqlite3, and
manages four tables.

**SQLite Schema:**

```sql
-- All conversation turns (persistent log)
conversation_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    role        TEXT NOT NULL,           -- "user" or "assistant"
    content     TEXT NOT NULL,
    created_at  REAL NOT NULL,
    summarized  INTEGER NOT NULL DEFAULT 0
)

-- Extracted facts with vector embeddings (sqlite-vec virtual table)
memory_facts USING vec0 (
    id          INTEGER PRIMARY KEY,
    embedding   float[768] distance_metric=cosine,
    +content    TEXT,                    -- auxiliary column
    +source     TEXT,
    +fact_type  TEXT,
    +created_at FLOAT                   -- FLOAT not REAL (vec0 parses REAL as vector type)
)

-- Conversation summaries
memory_summaries (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    content         TEXT NOT NULL,
    source_from_id  INTEGER NOT NULL,   -- first message id in summarized range
    source_to_id    INTEGER NOT NULL,   -- last message id in summarized range
    created_at      REAL NOT NULL,
    incorporated    INTEGER NOT NULL DEFAULT 0
)

-- Single-row base memory document
memory_base (
    id          INTEGER PRIMARY KEY CHECK(id = 1),
    content     TEXT NOT NULL DEFAULT '',
    updated_at  REAL NOT NULL
)
```

**Key methods:**

| Method | Purpose |
|--------|---------|
| `initialize()` | Open DB, load sqlite-vec, create tables, enable WAL mode |
| `append_message(role, content)` | Insert into conversation_log, returns row id |
| `get_recent_messages(n)` | Last n messages (oldest first), for deque reload |
| `count_unsummarized()` | Count of messages with `summarized = 0` |
| `get_unsummarized_messages()` | All unsummarized messages, oldest first |
| `mark_summarized(up_to_id)` | Mark messages <= id as summarized |
| `insert_fact(content, embedding, source, fact_type)` | Insert fact + vector |
| `search_facts(query_embedding, k)` | KNN cosine search, returns `list[FactRow]` |
| `delete_fact(fact_id)` | Remove a superseded fact |
| `insert_summary(content, from_id, to_id)` | Store a conversation summary |
| `count_unincorporated_summaries()` | Count summaries with `incorporated = 0` |
| `get_unincorporated_summaries()` | All unincorporated summaries |
| `mark_incorporated(up_to_id)` | Mark summaries <= id as incorporated |
| `get_base_memory()` | Read the single-row base memory document |
| `set_base_memory(content)` | Overwrite base memory |

**Vector serialization:** Embeddings are packed as raw bytes via
`struct.pack(f"{len(v)}f", *v)` as required by sqlite-vec.

**pysqlite3 fallback:** The module attempts `import pysqlite3 as sqlite3`
first (needed on macOS where the stock `sqlite3` module lacks extension loading
support), falling back to the standard library `sqlite3` on platforms where it
works.

### 4.4 `memory_manager.py`

**New file.** The `MemoryManager` class orchestrates retrieval, extraction, and
cascade operations. It receives `embed_fn` and `chat_fn` as constructor
callables, making it fully LLM-backend-agnostic.

**`retrieve_context(query) -> MemoryContext`** (synchronous, called before LLM)

1. Embed the user's query via `embed_fn` (~50ms with nomic-embed-text)
2. KNN search `memory_facts` for top-K matches (<1ms via sqlite-vec)
3. Filter results by minimum cosine similarity threshold
4. Fetch the base memory document
5. Format into a prompt block and return as `MemoryContext`

**`process_exchange(user_text, assistant_text)`** (background, called after response)

1. **Extract facts**: Call LLM with fact-extraction prompt, parse JSON array,
   embed each fact, check for near-duplicates, insert into vector store
2. **Short-term cascade**: If unsummarized message count >= threshold, summarize
   older messages via LLM, store summary, mark messages as summarized
3. **Long-term cascade**: If unincorporated summary count >= threshold, distill
   into base memory via LLM, overwrite base memory, mark summaries as incorporated

All three steps are wrapped in independent try/except blocks. A failure in any
step does not affect the others or crash the server.

---

## 5. Modified Files

### 5.1 `llm.py`

Three functions added after existing code:

```python
configure_embeddings(model: str = "nomic-embed-text") -> None
embed(text: str) -> list[float]           # single text -> 768-dim vector
embed_batch(texts: list[str]) -> list[list[float]]  # batch version
```

These are the **only two functions that touch the embedding backend**. When
migrating to vLLM, only these function bodies change. `MemoryManager` receives
`embed` as a callable and never imports `ollama` directly.

Both functions handle the Ollama API's dual response formats (object attributes
vs dict) the same way the existing `chat()` function does.

### 5.2 `conversation.py`

**Changes to `ConversationManager`:**

- `__init__` gains optional `memory_manager: MemoryManager | None` parameter
- `build_messages()` now injects memory context between the personality prompt
  and sensor state:
  1. Find last user message in the deque
  2. Call `memory_manager.retrieve_context(last_user_msg)`
  3. Append the formatted memory block to system content
  4. Then append sensor summary (unchanged)
- New `reload_from_db(db)` method: queries last 20 messages from
  `conversation_log` and populates the deque on startup
- New `_last_user_content()` helper: scans deque in reverse for the most
  recent user message

Memory retrieval errors are caught and logged as warnings -- they never
prevent the LLM from responding.

### 5.3 `server.py`

**Startup (lifespan function):**

```python
llm.configure_embeddings("nomic-embed-text")
memory_db.initialize()
memory_mgr = MemoryManager(db=memory_db, embed_fn=llm.embed, chat_fn=llm.chat)
conversation = ConversationManager(memory_manager=memory_mgr)
conversation.reload_from_db(memory_db)     # warm deque from DB
```

**`/chat/text` endpoint:**

- Added `BackgroundTasks` parameter
- After adding user message to deque: `memory_db.append_message("user", ...)`
- After getting LLM response: `memory_db.append_message("assistant", ...)`
- After returning response: `background_tasks.add_task(memory_mgr.process_exchange, ...)`

**`/chat/audio` endpoint:**

- Same persistence pattern as `/chat/text`
- Background `process_exchange` only runs for non-IGNORE actions (no point
  extracting facts from noise)
- All messages are persisted to DB regardless of action (for conversation log
  completeness)

### 5.4 `requirements.txt`

Added:

```
sqlite-vec>=0.1.1
pysqlite3>=0.5.4
```

---

## 6. Configuration

All tunables live in `memory_config.py` as a `MemoryConfig` dataclass. To
override defaults, modify the `memory_config = MemoryConfig()` line in
`server.py`:

```python
memory_config = MemoryConfig(
    retrieval_top_k=10,
    short_term_threshold=50,
)
```

No environment variables or config files are used. The dataclass is the single
source of truth.

---

## 7. Cascade Mechanics

### 7.1 Short-Term Cascade

**Trigger:** `count_unsummarized() >= short_term_threshold` (default: 30)

**Process:**
1. Fetch all unsummarized messages
2. Keep the most recent `short_term_keep` (default: 10) unsummarized
3. Format the older messages as a conversation transcript
4. Call LLM with summarization prompt
5. Store the summary in `memory_summaries` with source message id range
6. Mark the older messages as `summarized = 1`

**Result:** The conversation log continues to grow, but the unsummarized window
stays bounded. Summaries capture the gist of older conversations.

### 7.2 Long-Term Cascade

**Trigger:** `count_unincorporated_summaries() >= long_term_threshold` (default: 20)

**Process:**
1. Fetch all unincorporated summaries
2. Keep the most recent `long_term_keep` (default: 10) unincorporated
3. Fetch the current base memory document
4. Gather recent facts via a broad KNN query (top 10)
5. Call LLM with distillation prompt (current base + new summaries + facts)
6. Overwrite base memory with the LLM's output
7. Mark the older summaries as `incorporated = 1`

**Result:** The base memory document is a living, updated portrait of the user.
It is always injected into the system prompt, giving Lo-Bug persistent identity
awareness across all conversations.

---

## 8. Semantic Retrieval

The retrieval pipeline runs synchronously during `build_messages()`, adding
~50ms of latency per turn (dominated by the embedding call).

1. The user's latest message is embedded via `nomic-embed-text` (768 dims)
2. sqlite-vec performs exact KNN search over `memory_facts` using cosine distance
3. Results with cosine similarity below `retrieval_min_similarity` (default: 0.3)
   are filtered out (cosine distance > 0.7)
4. Up to `retrieval_top_k` (default: 5) facts are formatted as bullet points
   and injected into the system prompt

**Why KNN over full injection:** With hundreds of accumulated facts, injecting
all of them would bloat the prompt and dilute relevance. KNN ensures only the
most contextually relevant memories appear, keeping prompt size bounded and
signal-to-noise high.

---

## 9. Fact Deduplication

When a new fact is extracted, it is embedded and compared against existing facts:

1. KNN search with k=1 against the new fact's embedding
2. If the nearest existing fact has cosine distance < `duplicate_distance_threshold`
   (default: 0.15), the old fact is deleted
3. The new fact is always inserted

This handles fact updates naturally. If the user says "I moved to Seattle" after
previously saying "I live in Portland", the new fact's embedding will be close
enough to the old one to trigger replacement.

---

## 10. Setup

One-time setup after pulling the code:

```bash
cd server
source venv/bin/activate
pip install sqlite-vec pysqlite3
ollama pull nomic-embed-text
```

The database file (`lobug_memory.db`) is created automatically on first server
startup. To reset all memory:

```bash
rm server/lobug_memory.db
```

---

## 11. Verification

### Quick smoke test

```bash
# Start server
cd server && source venv/bin/activate && python server.py

# In another terminal:
curl -s http://localhost:8321/health | python -m json.tool
# Should show {"status": "ok", "ollama": true, ...}
```

### Persistence test

```bash
# Send a message
curl -s -X POST http://localhost:8321/chat/text \
    -H "Content-Type: application/json" \
    -d '{"text": "My name is Alex and I work at NASA"}'

# Wait 5s for background extraction
sleep 5

# Restart server (Ctrl-C, then python server.py again)

# Ask about the name -- memory should persist
curl -s -X POST http://localhost:8321/chat/text \
    -H "Content-Type: application/json" \
    -d '{"text": "Do you remember my name?"}'
```

### DB inspection

```bash
cd server && source venv/bin/activate
python -c "
from memory_db import MemoryDB
import llm
llm.configure_embeddings('nomic-embed-text')

db = MemoryDB()
db.initialize()

# Check conversation log
msgs = db.get_recent_messages(20)
print(f'{len(msgs)} messages in conversation_log')

# Check facts
facts = db.search_facts(llm.embed('user info'), k=20)
print(f'{len(facts)} facts stored')
for f in facts:
    print(f'  [{f.fact_type}] {f.content}')

# Check base memory
print(f'Base memory: {db.get_base_memory()[:200]}')
"
```

---

## 12. Design Decisions

**Why sqlite-vec over ChromaDB / FAISS / pgvector?**

Zero dependencies, single-file database, no server process, works on any
platform with SQLite. Perfect for a local-only embedded assistant. The entire
memory system lives in one `.db` file that can be backed up by copying it.

**Why pysqlite3?**

macOS ships a Python `sqlite3` module compiled without `enable_load_extension`,
which sqlite-vec requires. `pysqlite3` bundles its own SQLite build with
extension support. The import is conditional -- platforms with a capable stock
`sqlite3` don't need it.

**Why FLOAT instead of REAL in vec0 auxiliary columns?**

sqlite-vec's `vec0` virtual table parser interprets `REAL` as a vector type
(like `float[N]`), which causes a `chunk_size` error when used for auxiliary
(non-vector) columns. `FLOAT` is parsed as a standard SQL type and works
correctly.

**Why background tasks instead of async?**

Fact extraction involves 2-3 LLM calls (extraction + per-fact embedding). At
~2-5 seconds total, running this synchronously would delay the response.
FastAPI's `BackgroundTasks` lets us return the response immediately and process
memory asynchronously. Failures in background tasks are logged but never surface
to the user.

**Why not extract facts from IGNORE actions?**

If the LLM chose to ignore the input, it's likely background noise or
nonsensical fragments. Extracting facts from noise would pollute the memory
store with garbage.

**Why a single-row base memory table?**

The base memory is a living document -- not an append log. It gets rewritten
during each long-term cascade. A single row with `CHECK(id = 1)` enforces this
by schema.

---

## Files Created / Modified

| File | Action | Lines |
|------|--------|-------|
| `server/memory_config.py` | **Created** | 31 |
| `server/memory_prompts.py` | **Created** | 103 |
| `server/memory_db.py` | **Created** | 253 |
| `server/memory_manager.py` | **Created** | 227 |
| `server/llm.py` | Modified | +27 lines (embed functions) |
| `server/conversation.py` | Modified | +42 lines (memory injection, reload) |
| `server/server.py` | Modified | +37 lines (init, persistence, background tasks) |
| `server/requirements.txt` | Modified | +2 lines |
| `MEMORY.md` | **Created** | This document |

**Firmware:** No changes. The HTTP API contract is unchanged.
