# m5stackcore2-avatar-agent

Lo-Bug -- a local-only voice assistant living inside an M5Stack Core2. Everything
runs on-device and on a local Mac: speech-to-text (faster-whisper), LLM
(llama3.2:3b via Ollama), text-to-speech (macOS `say`), and persistent memory
(SQLite + sqlite-vec embeddings).

## Architecture

```
M5Stack Core2 (ESP32)              Mac (local server, port 8321)
 +-----------------------+          +---------------------------+
 | PDM Mic -> VAD -> WAV |--WiFi--->| STT (faster-whisper)      |
 | Wake detection        |          | LLM (Ollama llama3.2:3b)  |
 | IMU sensors           |--WiFi--->| Memory (sqlite-vec KNN)   |
 | Avatar face + speaker |<--WiFi---| TTS (macOS say)           |
 +-----------------------+          +---------------------------+
```

## Quick Start

```bash
# One-time setup
./scripts/setup.sh

# Edit WiFi credentials and server IP
nano firmware/config.h

# Start Ollama
ollama serve

# Start server
cd server && source venv/bin/activate && python server.py

# Build and flash firmware
pio run -t upload
```

## Documentation

- **[WAKE_WORD.md](WAKE_WORD.md)** -- Wake word detection design, amplitude-based
  Phase 1 implementation, and Edge Impulse TFLite Micro Phase 2 upgrade guide.
- **[MEMORY.md](MEMORY.md)** -- Persistent memory system: tiered architecture
  (working memory, conversation log, semantic facts, base memory), sqlite-vec
  KNN retrieval, fact extraction, summarization cascades.
