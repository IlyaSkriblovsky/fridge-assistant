from copy import deepcopy
from unittest.mock import Mock

import httpx
import pytest
from google.genai import errors, types

import assistant

pytestmark = pytest.mark.asyncio


def response(*parts):
    return types.GenerateContentResponse(candidates=[types.Candidate(
        content=types.Content(role="model", parts=list(parts))
    )])


async def test_text_and_audio_payload(gemini, wav):
    gemini.aio.models.generate_content.return_value = response(
        types.Part(text="  Купила молоко — γάλα.\n ")
    )
    assert await assistant.answer(gemini, wav, Mock()) == "Купила молоко — γάλα."
    kwargs = gemini.aio.models.generate_content.call_args.kwargs
    assert kwargs["model"] == assistant.MODEL
    audio = kwargs["contents"][0].parts[0].inline_data
    assert audio.data == wav
    assert audio.mime_type == "audio/wav"
    assert kwargs["config"].tools == assistant.TOOLS


@pytest.mark.parametrize("result", [
    types.GenerateContentResponse(),
    types.GenerateContentResponse(candidates=[]),
    types.GenerateContentResponse(candidates=[types.Candidate()]),
    response(types.Part(text=" \n")),
    response(),
])
async def test_missing_answer(gemini, result):
    gemini.aio.models.generate_content.return_value = result
    assert await assistant.answer(gemini, b"wav", Mock()) == assistant.NO_ANSWER


@pytest.mark.parametrize("count", [1, 2])
async def test_tool_round_preserves_model_content(gemini, monkeypatch, count):
    handler = Mock(side_effect=lambda items: {"items": items})
    monkeypatch.setattr(assistant, "HANDLERS", {"add_to_shopping_list": handler})
    model_reply = response(*[
        types.Part(function_call=types.FunctionCall(
            name="add_to_shopping_list", args={"items": [str(i)]}
        ), thought_signature=b"opaque-signature") for i in range(count)
    ])
    requests = []
    replies = iter([model_reply, response(types.Part(text="Готово"))])

    async def generate(**kwargs):
        # The production conversation list is mutated after each call.
        requests.append(deepcopy(kwargs["contents"]))
        return next(replies)

    gemini.aio.models.generate_content.side_effect = generate
    assert await assistant.answer(gemini, b"wav", Mock()) == "Готово"
    assert len(requests) == 2
    assert requests[1][1] == model_reply.candidates[0].content
    assert requests[1][1].parts[0].thought_signature == b"opaque-signature"
    tool_turn = requests[1][2]
    assert tool_turn.role == "user"
    assert [part.function_response.response for part in tool_turn.parts] == [
        {"items": [str(i)]} for i in range(count)
    ]
    assert handler.call_count == count


async def test_tool_errors(monkeypatch):
    monkeypatch.setattr(assistant, "HANDLERS", {"known": lambda items: {"items": items}})
    assert await assistant.call_tool(types.FunctionCall(name="missing")) == {
        "error": "unknown tool 'missing'"
    }
    result = await assistant.call_tool(types.FunctionCall(name="known", args={"wrong": 1}))
    assert "error" in result
    assert "wrong" in result["error"]


async def test_round_limit(gemini, monkeypatch):
    handler = Mock(return_value={"error": "Keep unavailable"})
    monkeypatch.setattr(assistant, "HANDLERS", {"known": handler})
    gemini.aio.models.generate_content.return_value = response(
        types.Part(function_call=types.FunctionCall(name="known", args={}))
    )
    assert await assistant.answer(gemini, b"wav", Mock()) == assistant.NO_ANSWER
    assert gemini.aio.models.generate_content.await_count == assistant.MAX_ROUNDS
    assert handler.call_count == assistant.MAX_ROUNDS
    contents = gemini.aio.models.generate_content.call_args.kwargs["contents"]
    assert contents[-1].parts[0].function_response.response == {"error": "Keep unavailable"}


@pytest.mark.parametrize("error", [errors.APIError(429, {}), httpx.ConnectError("offline")])
async def test_service_errors_propagate(gemini, error):
    gemini.aio.models.generate_content.side_effect = error
    with pytest.raises(type(error)) as caught:
        await assistant.answer(gemini, b"wav", Mock())
    assert caught.value is error
