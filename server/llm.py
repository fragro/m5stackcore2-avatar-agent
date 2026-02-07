"""Ollama LLM integration."""

import logging
import ollama

logger = logging.getLogger(__name__)

DEFAULT_MODEL = "llama3.2:3b"

_model_name: str = DEFAULT_MODEL


def configure(model: str = DEFAULT_MODEL) -> None:
    """Set which Ollama model to use."""
    global _model_name
    _model_name = model
    logger.info("LLM configured to use model: %s", _model_name)


def check_available() -> bool:
    """Check if Ollama is running and the model is available."""
    try:
        result = ollama.list()
        # Handle both old (.models) and new (dict) API styles
        if hasattr(result, 'models'):
            models_list = result.models
        elif isinstance(result, dict) and 'models' in result:
            models_list = result['models']
        else:
            models_list = result if isinstance(result, list) else []

        available = []
        for m in models_list:
            name = m.model if hasattr(m, 'model') else m.get('model', m.get('name', ''))
            available.append(name)

        if _model_name in available:
            return True
        base_name = _model_name.split(":")[0]
        return any(base_name in m for m in available)
    except Exception as e:
        logger.error("Ollama not available: %s", e)
        return False


def chat(messages: list[dict]) -> str:
    """Send messages to Ollama and get a response.

    Args:
        messages: List of message dicts with 'role' and 'content' keys.

    Returns:
        The assistant's response text.
    """
    try:
        response = ollama.chat(model=_model_name, messages=messages)
        # Handle both old (.message.content) and new (dict) API
        if hasattr(response, 'message'):
            text = response.message.content.strip()
        elif isinstance(response, dict):
            text = response.get('message', {}).get('content', '').strip()
        else:
            text = str(response).strip()
        logger.info("LLM response (%d chars): %s...", len(text), text[:80])
        return text
    except Exception as e:
        logger.error("LLM error: %s", e)
        return "Sorry, I'm having trouble thinking right now."
