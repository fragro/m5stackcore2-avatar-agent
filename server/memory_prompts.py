"""Prompt templates for fact extraction, summarization, and base-memory distillation."""


def fact_extraction_messages(user_text: str, assistant_text: str) -> list[dict]:
    """Build messages that instruct the LLM to extract facts from one exchange.

    Returns a message list ready for llm.chat().
    The LLM should output a JSON array of {"fact": "...", "type": "..."}.
    """
    return [
        {
            "role": "system",
            "content": (
                "You are a fact-extraction assistant. Given a conversation exchange "
                "between a user and an assistant, extract any new facts worth remembering "
                "about the user.\n\n"
                "Rules:\n"
                "- Output ONLY a JSON array. No commentary, no markdown fences.\n"
                "- Each element: {\"fact\": \"...\", \"type\": \"...\"}\n"
                "- Valid types: personal, preference, knowledge, event\n"
                "- Write each fact as a standalone third-person sentence "
                "(e.g. \"The user's name is Alex.\").\n"
                "- Be selective: skip chitchat, greetings, filler. Only extract "
                "things that would be useful to remember in future conversations.\n"
                "- If there are no facts worth extracting, output an empty array: []"
            ),
        },
        {
            "role": "user",
            "content": (
                f"User said: {user_text}\n"
                f"Assistant said: {assistant_text}\n\n"
                "Extract facts as a JSON array."
            ),
        },
    ]


def summarize_conversation_messages(conversation_text: str) -> list[dict]:
    """Build messages that instruct the LLM to summarize a batch of conversation turns.

    conversation_text should be a formatted block of turns, e.g.:
        User: ...
        Assistant: ...
    """
    return [
        {
            "role": "system",
            "content": (
                "You are a conversation summarizer. Condense the following "
                "conversation into a 3-5 sentence paragraph.\n\n"
                "Rules:\n"
                "- Use third person, past tense.\n"
                "- Preserve specific details: names, dates, numbers, decisions.\n"
                "- Omit greetings, filler, and small talk.\n"
                "- Output ONLY the summary paragraph, nothing else."
            ),
        },
        {
            "role": "user",
            "content": conversation_text,
        },
    ]


def distill_base_memory_messages(
    current_base: str, new_summaries: str, recent_facts: str
) -> list[dict]:
    """Build messages that instruct the LLM to integrate new information into base memory.

    The LLM should produce an updated base-memory document organized into sections.
    """
    return [
        {
            "role": "system",
            "content": (
                "You are a memory manager for a voice assistant called Lo-Bug. "
                "Your job is to maintain a concise document that captures everything "
                "important about the user.\n\n"
                "You will receive:\n"
                "1. The current base memory (may be empty on first run)\n"
                "2. New conversation summaries\n"
                "3. Recently extracted facts\n\n"
                "Rules:\n"
                "- Integrate the new information into the existing base memory.\n"
                "- Organize into sections: User Profile, Preferences, "
                "Ongoing Topics, Key History.\n"
                "- Remove outdated or contradicted information.\n"
                "- Keep the total document under 400 words.\n"
                "- Output ONLY the updated base memory document, nothing else."
            ),
        },
        {
            "role": "user",
            "content": (
                f"=== Current base memory ===\n{current_base or '(empty)'}\n\n"
                f"=== New conversation summaries ===\n{new_summaries}\n\n"
                f"=== Recent facts ===\n{recent_facts}\n\n"
                "Produce the updated base memory document."
            ),
        },
    ]
