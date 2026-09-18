# /// script
# requires-python = ">=3.12"
# dependencies = ["google-genai"]
# ///
"""Try Gemini on real recordings from the device, without the Live API.

Each WAV goes to generate_content whole, together with a stub shopping-list
tool. The script runs the tool-call loop by hand, the way the server would,
and prints what the model called, what it answered, how long each round took
and how many tokens it cost. Nothing is added anywhere: the tool only prints.

    export GEMINI_API_KEY=...            # from aistudio.google.com
    uv run experiments/gemini_audio.py   # every recordings/*.wav
    uv run experiments/gemini_audio.py --model gemini-3.8-flash recordings/x.wav
"""

from __future__ import annotations

import argparse
import os
import sys
import time
import wave
from pathlib import Path

from google import genai
from google.genai import errors, types

DEFAULT_MODEL = "gemini-3.5-flash-lite"
RECORDINGS_DIR = Path(__file__).resolve().parent.parent / "recordings"

# A model that keeps calling tools should not loop forever on a test run.
MAX_ROUNDS = 5

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
        "list on their phones in the store and ticks items off there."
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

TOOLS = [types.Tool(function_declarations=[ADD_TO_SHOPPING_LIST])]


def add_to_shopping_list(items: list[str]) -> dict[str, object]:
    """Stub: pretend the items went onto the list."""
    return {"status": "ok", "added": items}


HANDLERS = {"add_to_shopping_list": add_to_shopping_list}


def call_tool(call: types.FunctionCall) -> dict[str, object]:
    handler = HANDLERS.get(call.name or "")
    if handler is None:
        return {"error": f"unknown tool {call.name!r}"}
    try:
        return handler(**(call.args or {}))
    except TypeError as exc:  # the model passed arguments the tool does not take
        return {"error": str(exc)}


def wav_duration(path: Path) -> str:
    try:
        with wave.open(str(path), "rb") as wav:
            return f"{wav.getnframes() / wav.getframerate():.1f} s"
    except (wave.Error, EOFError):
        return "unreadable WAV header"


def describe_usage(response: types.GenerateContentResponse) -> str:
    usage = response.usage_metadata
    if usage is None:
        return "no usage data"
    return (
        f"in {usage.prompt_token_count or 0}, "
        f"out {usage.candidates_token_count or 0}, "
        f"thinking {usage.thoughts_token_count or 0}"
    )


def run(client: genai.Client, model: str, path: Path) -> None:
    print(f"=== {path.name}  ({wav_duration(path)})")
    contents: list[types.Content] = [
        types.Content(
            role="user",
            parts=[types.Part.from_bytes(data=path.read_bytes(), mime_type="audio/wav")],
        )
    ]
    config = types.GenerateContentConfig(
        system_instruction=SYSTEM_INSTRUCTION,
        tools=TOOLS,
    )

    started = time.monotonic()
    for round_no in range(1, MAX_ROUNDS + 1):
        round_started = time.monotonic()
        response = client.models.generate_content(
            model=model, contents=contents, config=config
        )
        took = time.monotonic() - round_started
        print(f"  round {round_no}: {took:.2f} s, tokens {describe_usage(response)}")

        if not response.candidates or response.candidates[0].content is None:
            print(f"  !! no answer: {response.prompt_feedback or response.candidates}")
            return
        # The model's turn goes back as is: on newer models it carries thought
        # signatures that the next round needs.
        contents.append(response.candidates[0].content)

        calls = response.function_calls
        if not calls:
            print(f"  reply: {response.text}")
            break

        results = []
        for call in calls:
            result = call_tool(call)
            print(f"  -> {call.name}({call.args})  => {result}")
            results.append(
                types.Part.from_function_response(name=call.name, response=result)
            )
        # Function results go back as a user turn: the Gemini API rejects "tool".
        contents.append(types.Content(role="user", parts=results))
    else:
        print(f"  !! still calling tools after {MAX_ROUNDS} rounds, gave up")

    print(f"  total: {time.monotonic() - started:.2f} s")


def main() -> int:
    parser = argparse.ArgumentParser(description="Try Gemini on device recordings.")
    parser.add_argument("files", nargs="*", type=Path, help="WAV files (default: recordings/*.wav)")
    parser.add_argument("--model", default=DEFAULT_MODEL, help=f"default: {DEFAULT_MODEL}")
    args = parser.parse_args()

    files = args.files or sorted(RECORDINGS_DIR.glob("*.wav"))
    if not files:
        print(f"No recordings in {RECORDINGS_DIR}", file=sys.stderr)
        return 1

    if not (os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")):
        print("Set GEMINI_API_KEY (get one at aistudio.google.com)", file=sys.stderr)
        return 1
    client = genai.Client()  # picks the key up from the environment
    print(f"model: {args.model}\n")
    for path in files:
        try:
            run(client, args.model, path)
        except errors.APIError as exc:
            print(f"  !! API error: {exc}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
