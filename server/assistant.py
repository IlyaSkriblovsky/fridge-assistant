"""The assistant itself: a recording in, the text for the screen out.

The recording goes to Gemini's generateContent whole, together with the tools
the assistant may use. The model makes out the speech, decides what to do and
calls tools; the server runs the calls and sends the results back, round after
round, until the model answers with text instead.
"""

from __future__ import annotations

import asyncio
import os
import time
from collections.abc import Callable

from google import genai
from google.genai import types

import shopping_list
import reminders
from datetime import datetime, timezone

MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.5-flash-lite")

# A model that keeps calling tools must not hold the device until it times out.
MAX_ROUNDS = 5

# Said when the model gave no text to show. The device has no other way to
# learn that something went wrong (see docs/deferred.md).
NO_ANSWER = "Не получилось ответить, попробуй ещё раз."

SYSTEM_INSTRUCTION = """\
You are a voice assistant on a small e-paper device on a family's fridge.
Each request is one recording of the user speaking, usually in Russian.
Do what they ask using the tools, then reply with one or two short sentences
in the language they spoke, saying what you did. The reply is shown on the
device's screen, not spoken. If you could not make out the request, or no tool
fits it, say so briefly instead of guessing.
"""

ADD_TO_SHOPPING_LIST = types.FunctionDeclaration(
    name="add_to_shopping_list",
    description=(
        "Add items to the family's shared shopping list. The family reads the "
        "list on their phones in the store and ticks items off there. Returns "
        "what happened to each item: `added`; `already_listed`, when it was "
        "on the list already and nothing changed; or `restored`, when it was "
        "on the list ticked off and is now unticked again."
    ),
    parameters_json_schema={
        "type": "object",
        "properties": {
            "items": {
                "type": "array",
                "items": {"type": "string"},
                "description": (
                    "One entry per item, as it should appear on the list: "
                    "in the user's language, in the nominative case, with the "
                    "quantity if the user gave one."
                ),
            }
        },
        "required": ["items"],
    },
)

CREATE_REMINDER = types.FunctionDeclaration(
    name="create_reminder", description="Create a one-shot reminder. Extract duration, never calculate its date yourself. Use local_at only for an explicit calendar date and time in Asia/Nicosia.",
    parameters_json_schema={"type": "object", "properties": {
        "text": {"type": "string", "description": "Only the essential subject or action to remember, without reminder-request wording or its introductory prepositions; at most 240 UTF-8 bytes."},
        "amount": {"type": "integer", "minimum": 1},
        "unit": {"type": "string", "enum": ["seconds", "minutes", "hours", "days", "weeks", "months", "years"]},
        "local_at": {"type": "string", "description": "Exact local YYYY-MM-DDTHH:MM:SS, without offset."}},
        "required": ["text"], "additionalProperties": False})
LIST_REMINDERS = types.FunctionDeclaration(name="list_reminders", description="List all active reminders, including overdue ones, with IDs and due times.", parameters_json_schema={"type": "object", "properties": {}})
CANCEL_REMINDER = types.FunctionDeclaration(name="cancel_reminder", description="Cancel one reminder by ID obtained from list_reminders.", parameters_json_schema={"type": "object", "properties": {"id": {"type": "integer"}}, "required": ["id"]})
TOOLS = [types.Tool(function_declarations=[ADD_TO_SHOPPING_LIST, CREATE_REMINDER, LIST_REMINDERS, CANCEL_REMINDER])]

# Tools are synchronous and run in a worker thread each.
HANDLERS: dict[str, Callable[..., dict[str, object]]] = {
    "add_to_shopping_list": shopping_list.add,
    "create_reminder": reminders.create,
    "list_reminders": reminders.list_active,
    "cancel_reminder": reminders.cancel,
}


async def call_tool(call: types.FunctionCall) -> dict[str, object]:
    handler = HANDLERS.get(call.name or "")
    if handler is None:
        return {"error": f"unknown tool {call.name!r}"}
    try:
        return await asyncio.to_thread(handler, **(call.args or {}))
    except TypeError as exc:  # the model passed arguments the tool does not take
        return {"error": str(exc)}


def describe_usage(response: types.GenerateContentResponse) -> str:
    usage = response.usage_metadata
    if usage is None:
        return "no usage data"
    return (
        f"in {usage.prompt_token_count or 0}, "
        f"out {usage.candidates_token_count or 0}, "
        f"thinking {usage.thoughts_token_count or 0}"
    )


async def answer(client: genai.Client, wav: bytes, log: Callable[[str], None]) -> str:
    token = reminders.REQUEST_TIME.set(reminders.REQUEST_TIME.get() or datetime.now(timezone.utc))
    try:
        return await _answer(client, wav, log)
    finally:
        reminders.REQUEST_TIME.reset(token)


async def _answer(client: genai.Client, wav: bytes, log: Callable[[str], None]) -> str:
    """Carry out what the recording asks for and return the reply for the screen.

    Errors from Gemini itself are left to the caller. A failing tool is not an
    error here: its result says so, and the model tells the user.
    """
    contents: list[types.Content] = [
        types.Content(
            role="user", parts=[types.Part.from_bytes(data=wav, mime_type="audio/wav")]
        )
    ]
    reference = reminders.REQUEST_TIME.get() or datetime.now(timezone.utc)
    instruction = SYSTEM_INSTRUCTION + """
For reminders, only confirm success reported by a tool; include the resulting local date/time.
The reminder text must contain only the essential subject or action. Remove request framing
such as 'напомни мне', 'про', 'о', 'об', and the scheduling phrase; put the subject in its
natural standalone form. For example, 'напомни мне про бутерброд через минуту' becomes
text='бутерброд', amount=1, unit='minutes'; 'напомни о встрече' becomes text='встреча'.
Preserve prepositions that belong to the meaning: 'снять яйца с плиты', 'позвонить маме
по поводу билетов', 'фильтр для воды'. Do not mechanically remove every preposition.
For cancellation by meaning, first list reminders. If several match, cancel none and ask
for a new command specifying the time. Each recording is independent, with no hidden session.
Ask for a precise time for vague requests such as 'in the morning'. Never guess ambiguous
calendar times or work around a DST ambiguity error. Use amount/unit for relative durations;
months and years are calendar arithmetic performed by the server.
""" + f"\nRequest time: {reference.astimezone(reminders.ZONE).isoformat()} (Asia/Nicosia)."
    # The request task owns this context; worker threads inherit the fixed anchor.
    config = types.GenerateContentConfig(system_instruction=instruction, tools=TOOLS)

    for round_no in range(1, MAX_ROUNDS + 1):
        started = time.monotonic()
        response = await client.aio.models.generate_content(
            model=MODEL, contents=contents, config=config
        )
        log(
            f"gemini round {round_no}: {time.monotonic() - started:.2f} s, "
            f"tokens {describe_usage(response)}"
        )

        if not response.candidates or response.candidates[0].content is None:
            log(f"no answer: {response.prompt_feedback or response.candidates}")
            return NO_ANSWER
        # The model's turn goes back as is: on newer models it carries thought
        # signatures that the next round needs.
        contents.append(response.candidates[0].content)

        calls = response.function_calls
        if not calls:
            return (response.text or "").strip() or NO_ANSWER

        results = []
        for call in calls:
            started = time.monotonic()
            result = await call_tool(call)
            log(f"-> {call.name}({call.args}) => {result}  {time.monotonic() - started:.2f} s")
            results.append(types.Part.from_function_response(name=call.name, response=result))
        # Function results go back as a user turn: the Gemini API rejects "tool".
        contents.append(types.Content(role="user", parts=results))

    log(f"still calling tools after {MAX_ROUNDS} rounds, gave up")
    return NO_ANSWER
