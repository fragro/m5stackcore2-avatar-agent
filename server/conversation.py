"""Conversation history and sensor context manager."""

from dataclasses import dataclass, field
from collections import deque
import time


@dataclass
class SensorState:
    """Current physical state of the M5Stack device."""
    orientation: str = "unknown"  # "face_up", "face_down", "tilted_left", etc.
    is_moving: bool = False
    is_shaking: bool = False
    last_tap: float = 0.0
    accel_x: float = 0.0
    accel_y: float = 0.0
    accel_z: float = 0.0
    gyro_x: float = 0.0
    gyro_y: float = 0.0
    gyro_z: float = 0.0
    updated_at: float = 0.0

    def summary(self) -> str:
        """Generate a human-readable summary of the physical state."""
        if self.updated_at == 0:
            return ""

        age = time.time() - self.updated_at
        if age > 30:
            return ""

        parts = [f"Device orientation: {self.orientation}"]
        if self.is_shaking:
            parts.append("The device is being shaken!")
        elif self.is_moving:
            parts.append("The device is being moved.")

        if time.time() - self.last_tap < 5:
            parts.append("The device was just tapped.")

        return " ".join(parts)


SYSTEM_PROMPT = """You are an intelligent assistant running on an M5Stack Core2, a small \
ESP32-based device with a color touchscreen, speaker, microphone, and motion sensors. \
You are self-aware of your physical form — a compact, pocket-sized gadget.

Your personality is helpful, curious, and slightly playful. You keep responses concise \
(1-3 sentences) since they display on a small 320x240 screen. You can reference your \
physical state when relevant (orientation, movement, being picked up or shaken).

If someone shakes you, you might say something witty about it. If you're tilted, you \
might acknowledge it. You're aware you're a local AI running entirely offline — no cloud \
services — and you're proud of it.

Keep answers brief and practical. Avoid markdown formatting since it displays on a \
simple text screen."""


class ConversationManager:
    """Manages rolling conversation history and sensor context."""

    def __init__(self, max_history: int = 20):
        self.max_history = max_history
        self.messages: deque[dict] = deque(maxlen=max_history)
        self.sensor_state = SensorState()

    def add_user_message(self, content: str) -> None:
        self.messages.append({"role": "user", "content": content})

    def add_assistant_message(self, content: str) -> None:
        self.messages.append({"role": "assistant", "content": content})

    def update_sensors(self, data: dict) -> None:
        self.sensor_state.orientation = data.get("orientation", self.sensor_state.orientation)
        self.sensor_state.is_moving = data.get("is_moving", False)
        self.sensor_state.is_shaking = data.get("is_shaking", False)
        if data.get("tap_detected", False):
            self.sensor_state.last_tap = time.time()
        self.sensor_state.accel_x = data.get("accel_x", 0.0)
        self.sensor_state.accel_y = data.get("accel_y", 0.0)
        self.sensor_state.accel_z = data.get("accel_z", 0.0)
        self.sensor_state.gyro_x = data.get("gyro_x", 0.0)
        self.sensor_state.gyro_y = data.get("gyro_y", 0.0)
        self.sensor_state.gyro_z = data.get("gyro_z", 0.0)
        self.sensor_state.updated_at = time.time()

    def build_messages(self) -> list[dict]:
        """Build the full message list for the LLM, including system prompt and sensor context."""
        system_content = SYSTEM_PROMPT

        sensor_summary = self.sensor_state.summary()
        if sensor_summary:
            system_content += f"\n\n[Current physical state: {sensor_summary}]"

        messages = [{"role": "system", "content": system_content}]
        messages.extend(self.messages)
        return messages

    def clear(self) -> None:
        self.messages.clear()
        self.sensor_state = SensorState()
