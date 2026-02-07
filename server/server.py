"""FastAPI server for M5Stack Intelligent Agent."""

import base64
import logging
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File
from fastapi.responses import JSONResponse
from pydantic import BaseModel

import stt
import llm
import tts
from conversation import ConversationManager

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
)
logger = logging.getLogger(__name__)

conversation = ConversationManager()


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Initialize models on startup."""
    logger.info("Starting M5Stack Agent Server...")
    stt.load_model("base")
    llm.configure("llama3.2:3b")
    tts.load_model()

    if not llm.check_available():
        logger.warning("Ollama not available! Make sure 'ollama serve' is running.")

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
    audio_b64: str  # Base64-encoded WAV

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
async def chat_text(req: TextRequest):
    """Text chat: accept text, return LLM response."""
    logger.info("Text chat: '%s'", req.text)

    conversation.add_user_message(req.text)
    messages = conversation.build_messages()
    response_text = llm.chat(messages)
    conversation.add_assistant_message(response_text)

    return TextResponse(response=response_text)


@app.post("/chat/audio", response_model=AudioResponse)
async def chat_audio(audio: UploadFile = File(...)):
    """Voice chat: accept WAV audio, return transcription + response + TTS audio."""
    audio_bytes = await audio.read()
    logger.info("Audio chat: received %d bytes", len(audio_bytes))

    # Speech to text
    transcription = stt.transcribe(audio_bytes)
    if not transcription:
        return AudioResponse(
            transcription="",
            response="I couldn't hear anything. Could you try again?",
            audio_b64=base64.b64encode(tts.synthesize("I couldn't hear anything. Could you try again?")).decode(),
        )

    # LLM response
    conversation.add_user_message(transcription)
    messages = conversation.build_messages()
    response_text = llm.chat(messages)
    conversation.add_assistant_message(response_text)

    # Text to speech
    audio_response = tts.synthesize(response_text)
    audio_b64 = base64.b64encode(audio_response).decode()

    return AudioResponse(
        transcription=transcription,
        response=response_text,
        audio_b64=audio_b64,
    )


@app.post("/context/sensors")
async def update_sensors(data: SensorData):
    """Update sensor context for the LLM."""
    conversation.update_sensors(data.model_dump())
    return {"status": "ok"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8321)
