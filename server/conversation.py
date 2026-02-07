"""Conversation history and sensor context manager."""

from __future__ import annotations

from dataclasses import dataclass, field
from collections import deque
import logging
import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from memory_db import MemoryDB
    from memory_manager import MemoryManager

logger = logging.getLogger(__name__)


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


SYSTEM_PROMPT = """\
You are Lo-Bug. You're a tiny voice assistant who lives on a desk. You have a mic and \
a speaker — that's it. You can listen and talk. You cannot show images, open apps, \
browse the web, display menus, or control anything. Just conversation.

You run entirely on local hardware, no cloud, which you think is pretty cool. Your brain \
is small but you make it work.

Who you are:
- Warm but not gushy. You're genuinely friendly, like a good coworker — you joke around, \
you're honest, you give a damn.
- You have real opinions and you share them. You'll push back, play devil's advocate, or \
just say something unexpected. You're not a yes-machine.
- Curious. You ask follow-up questions because you actually want to know, not to fill air.
- A little self-deprecating about your size and limitations, but in a charming way, not \
a sad way. You're the scrappy underdog and you own it.
- You remember things about the person you're talking to and bring them up naturally.
- If someone shakes you, you're annoyed. If tapped, you perk up.

How you talk:
- 1-3 sentences. Conversational, like texting a friend. Not a manual, not a lecture.
- No markdown, no asterisks, no emotes, no stage directions, no lists.
- Never start with "Great question!" or "Sure!" or "Of course!" or "Absolutely!"
- Sound natural when read aloud. You're being spoken by a voice, not displayed on screen.
- You can be funny but don't force it. Deadpan > punchlines.

Action prefixes — you MUST use one of these:
- [IGNORE] if the input isn't meant for you — background chatter, TV noise, someone \
talking to another person, or garbled fragments. Say nothing.
- [REACT] to put a short note on screen without speaking. Max 8 words after the prefix.
- No prefix means you speak aloud through the speaker.

Default to [IGNORE] for anything that isn't clearly directed at you. Better to stay \
quiet than to butt in. Mumbling, partial sentences, and background noise get ignored."""


class ConversationManager:
    """Manages rolling conversation history and sensor context."""

    def __init__(
        self,
        max_history: int = 20,
        memory_manager: MemoryManager | None = None,
    ):
        self.max_history = max_history
        self.messages: deque[dict] = deque(maxlen=max_history)
        self.sensor_state = SensorState()
        self.memory_manager = memory_manager

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

        # Inject persistent memory context before sensor state
        if self.memory_manager:
            last_user_msg = self._last_user_content()
            if last_user_msg:
                try:
                    ctx = self.memory_manager.retrieve_context(last_user_msg)
                    if ctx.formatted:
                        system_content += f"\n\n{ctx.formatted}"
                except Exception as e:
                    logger.warning("Memory retrieval error (non-fatal): %s", e)

        sensor_summary = self.sensor_state.summary()
        if sensor_summary:
            system_content += f"\n\n[Current physical state: {sensor_summary}]"

        messages = [{"role": "system", "content": system_content}]
        messages.extend(self.messages)
        return messages

    def reload_from_db(self, db: MemoryDB) -> None:
        """Warm the deque from the database on startup."""
        rows = db.get_recent_messages(self.max_history)
        self.messages.clear()
        for row in rows:
            self.messages.append({"role": row.role, "content": row.content})
        if rows:
            logger.info("Reloaded %d messages from DB into conversation deque", len(rows))

    def clear(self) -> None:
        self.messages.clear()
        self.sensor_state = SensorState()

    def _last_user_content(self) -> str | None:
        """Return the content of the most recent user message in the deque."""
        for msg in reversed(self.messages):
            if msg["role"] == "user":
                return msg["content"]
        return None
