"""FastAPI server for M5Stack Intelligent Agent."""

import base64
import logging
import time
from contextlib import asynccontextmanager

from fastapi import BackgroundTasks, FastAPI, UploadFile, File
from fastapi.responses import JSONResponse
from pydantic import BaseModel

import stt
import llm
import tts
from conversation import ConversationManager
from decision import heuristic_filter, parse_llm_response, Action
from memory_config import MemoryConfig
from memory_db import MemoryDB
from memory_manager import MemoryManager

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
)
logger = logging.getLogger(__name__)

# Persistent memory subsystem (initialized in lifespan)
memory_config = MemoryConfig()
memory_db = MemoryDB(memory_config)
memory_mgr: MemoryManager | None = None
conversation: ConversationManager | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Initialize models and memory system on startup."""
    global memory_mgr, conversation

    logger.info("Starting M5Stack Agent Server...")
    stt.load_model("base")
    llm.configure("llama3.2:3b")
    llm.configure_embeddings(memory_config.embedding_model)
    tts.load_model()

    if not llm.check_available():
        logger.warning("Ollama not available! Make sure 'ollama serve' is running.")

    # Initialize persistent memory
    memory_db.initialize()
    memory_mgr = MemoryManager(
        db=memory_db, embed_fn=llm.embed, chat_fn=llm.chat, config=memory_config
    )
    conversation = ConversationManager(memory_manager=memory_mgr)
    conversation.reload_from_db(memory_db)

    logger.info("Server ready.")
    yield
    logger.info("Server shutting down.")


app = FastAPI(title="M5Stack Agent Server", lifespan=lifespan)


# --- Request/Response Models ---

class TextRequest(BaseModel):
    text: str

class TextResponse(BaseModel):
    response: str

class AudioResponse(BaseModel):
    transcription: str
    response: str
    action: str = "respond"  # "respond", "ignore", "react"
    audio_b64: str

class SensorData(BaseModel):
    orientation: str = "unknown"
    is_moving: bool = False
    is_shaking: bool = False
    tap_detected: bool = False
    accel_x: float = 0.0
    accel_y: float = 0.0
    accel_z: float = 0.0
    gyro_x: float = 0.0
    gyro_y: float = 0.0
    gyro_z: float = 0.0


# --- Endpoints ---

@app.get("/health")
async def health():
    """Health check for M5Stack connection verification."""
    ollama_ok = llm.check_available()
    return {
        "status": "ok",
        "ollama": ollama_ok,
        "timestamp": time.time(),
    }


@app.post("/chat/text", response_model=TextResponse)
async def chat_text(req: TextRequest, background_tasks: BackgroundTasks):
    """Text chat: accept text, return LLM response."""
    logger.info("Text chat: '%s'", req.text)

    conversation.add_user_message(req.text)
    memory_db.append_message("user", req.text)

    messages = conversation.build_messages()
    response_text = llm.chat(messages)

    conversation.add_assistant_message(response_text)
    memory_db.append_message("assistant", response_text)

    # Background: extract facts and run cascade checks
    background_tasks.add_task(memory_mgr.process_exchange, req.text, response_text)

    return TextResponse(response=response_text)


@app.post("/chat/audio", response_model=AudioResponse)
async def chat_audio(
    audio: UploadFile = File(...),
    background_tasks: BackgroundTasks = BackgroundTasks(),
):
    """Voice chat: accept WAV audio, return transcription + response + TTS audio."""
    audio_bytes = await audio.read()
    logger.info("Audio chat: received %d bytes", len(audio_bytes))

    # Speech to text (returns rich metadata)
    result = stt.transcribe(audio_bytes)

    # Empty transcription → silent ignore (no "I couldn't hear anything")
    if not result.text:
        logger.info("Empty transcription, ignoring silently")
        return AudioResponse(
            transcription="", response="", action="ignore", audio_b64="",
        )

    # Heuristic pre-filter (no LLM call, no conversation history pollution)
    filter_reason = heuristic_filter(result)
    if filter_reason:
        logger.info("Heuristic filtered ('%s'): %s", result.text, filter_reason)
        return AudioResponse(
            transcription=result.text, response="", action="ignore", audio_b64="",
        )

    # Passed filters → add to conversation and get LLM response
    conversation.add_user_message(result.text)
    memory_db.append_message("user", result.text)

    messages = conversation.build_messages()
    raw_response = llm.chat(messages)

    # Parse LLM action prefix
    action, cleaned_text = parse_llm_response(raw_response)

    # Store in conversation history (LLM decisions are context, even ignores)
    conversation.add_assistant_message(raw_response)
    memory_db.append_message("assistant", raw_response)

    # Background: extract facts for non-IGNORE actions
    if action != Action.IGNORE:
        background_tasks.add_task(
            memory_mgr.process_exchange, result.text, cleaned_text
        )

    if action == Action.IGNORE:
        logger.info("LLM chose to ignore: '%s'", result.text)
        return AudioResponse(
            transcription=result.text, response=cleaned_text,
            action="ignore", audio_b64="",
        )

    if action == Action.REACT:
        logger.info("LLM chose react-only: '%s'", cleaned_text)
        return AudioResponse(
            transcription=result.text, response=cleaned_text,
            action="react", audio_b64="",
        )

    # Normal response → synthesize speech
    audio_response = tts.synthesize(cleaned_text)
    audio_b64 = base64.b64encode(audio_response).decode()

    return AudioResponse(
        transcription=result.text, response=cleaned_text,
        action="respond", audio_b64=audio_b64,
    )


@app.post("/context/sensors")
async def update_sensors(data: SensorData):
    """Update sensor context for the LLM."""
    conversation.update_sensors(data.model_dump())
    return {"status": "ok"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8321)
