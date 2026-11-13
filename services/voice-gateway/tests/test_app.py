import asyncio
import json
import time
from collections.abc import AsyncIterator

import pytest
from fastapi.testclient import TestClient

from micropixel_voice_gateway.adapters import ASRAdapter, ASRSession, LLMAdapter, MockASRAdapter, MockTTSAdapter
from micropixel_voice_gateway.app import Dependencies, create_app
from micropixel_voice_gateway.codec import OpusCodec
from micropixel_voice_gateway.protocol import PROTOCOL_VERSION, AudioFrame, AudioKind


HELLO = {
    "type": "hello",
    "protocol": PROTOCOL_VERSION,
    "codec": "opus",
    "sample_rate": 16_000,
    "channels": 1,
    "frame_duration_ms": 20,
}


def test_health() -> None:
    with TestClient(create_app()) as client:
        assert client.get("/healthz").json() == {"status": "ok"}


def test_gateway_requires_device_token(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VOICE_DEVICE_TOKEN", raising=False)
    with pytest.raises(RuntimeError, match="VOICE_DEVICE_TOKEN is required"):
        create_app()


def test_websocket_rejects_invalid_negotiation() -> None:
    with TestClient(create_app()) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json({"type": "hello", "protocol": 1, "codec": "pcm"})
            assert socket.receive_json()["type"] == "error"


def test_context_clear_is_acknowledged_only_while_idle() -> None:
    with TestClient(create_app()) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "context.clear"})
            assert socket.receive_json()["type"] == "context.cleared"

            socket.send_json({"type": "session.start", "session_id": 9, "generation": 2})
            assert socket.receive_json()["type"] == "session.started"
            socket.send_json({"type": "context.clear"})
            while True:
                error = socket.receive_json()
                if error["type"] == "error":
                    break
            assert error["type"] == "error"
            assert error["code"] == "protocol_error"


def test_mock_voice_session_streams_text_and_opus() -> None:
    session_id = 17
    generation = 4
    with TestClient(create_app()) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "session.start", "session_id": session_id, "generation": generation})
            started = socket.receive_json()
            assert (started["type"], started["session_id"], started["generation"]) == (
                "session.started",
                session_id,
                generation,
            )
            codec = OpusCodec()
            packet = codec.encode(bytes(codec.frame_samples * 2))
            socket.send_bytes(AudioFrame(AudioKind.INPUT_OPUS, session_id, generation, 0, 0, packet).encode())
            socket.send_json({"type": "input.commit", "session_id": session_id, "generation": generation})

            event_types: list[str] = []
            audio_frames = 0
            while "response.audio.stop" not in event_types:
                message = socket.receive()
                if message.get("bytes") is not None:
                    frame = AudioFrame.decode(message["bytes"])
                    assert frame.kind == AudioKind.OUTPUT_OPUS
                    assert (frame.session_id, frame.generation) == (session_id, generation)
                    audio_frames += 1
                elif message.get("text") is not None:
                    event = json.loads(message["text"])
                    assert (event["session_id"], event["generation"]) == (session_id, generation)
                    event_types.append(event["type"])

            assert "transcript.snapshot" in event_types
            assert "transcript.final" in event_types
            assert "response.text.delta" in event_types
            assert "response.text.final" in event_types
            assert audio_frames >= 3


class SlowLLMAdapter(LLMAdapter):
    async def stream(self, turns: tuple[tuple[str, str], ...], prompt: str) -> AsyncIterator[str]:
        del turns, prompt
        yield "第一段"
        await asyncio.sleep(30)


class SlowOpenASRAdapter(ASRAdapter):
    async def open(self) -> ASRSession:
        await asyncio.sleep(30)
        raise AssertionError("cancel should interrupt ASR startup")


class FailIfCalledLLMAdapter(LLMAdapter):
    async def stream(self, turns: tuple[tuple[str, str], ...], prompt: str) -> AsyncIterator[str]:
        del turns, prompt
        raise AssertionError("LLM must not run for an empty transcript")
        yield ""  # pragma: no cover


def test_cancel_preempts_response_and_allows_next_generation() -> None:
    dependencies = Dependencies(MockASRAdapter(), SlowLLMAdapter(), MockTTSAdapter())
    with TestClient(create_app(dependencies)) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "session.start", "session_id": 31, "generation": 8})
            assert socket.receive_json()["type"] == "session.started"
            codec = OpusCodec()
            packet = codec.encode(bytes(codec.frame_samples * 2))
            socket.send_bytes(AudioFrame(AudioKind.INPUT_OPUS, 31, 8, 0, 0, packet).encode())
            socket.send_json({"type": "input.commit", "session_id": 31, "generation": 8})

            while True:
                message = socket.receive()
                if message.get("bytes") is not None:
                    break

            cancel_started = time.monotonic()
            socket.send_json({"type": "session.cancel", "session_id": 31, "generation": 8})
            while True:
                event = socket.receive_json()
                if event["type"] == "session.cancelled":
                    break
            assert time.monotonic() - cancel_started < 1.0
            assert (event["session_id"], event["generation"]) == (31, 8)

            socket.send_json({"type": "session.start", "session_id": 32, "generation": 9})
            next_started = socket.receive_json()
            assert (next_started["type"], next_started["session_id"], next_started["generation"]) == (
                "session.started",
                32,
                9,
            )


def test_cancel_before_session_task_starts_is_acknowledged() -> None:
    dependencies = Dependencies(SlowOpenASRAdapter(), SlowLLMAdapter(), MockTTSAdapter())
    with TestClient(create_app(dependencies)) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "session.start", "session_id": 40, "generation": 2})
            socket.send_json({"type": "session.cancel", "session_id": 40, "generation": 2})

            event = socket.receive_json()
            assert (event["type"], event["session_id"], event["generation"]) == (
                "session.cancelled",
                40,
                2,
            )


def test_stale_audio_and_commit_do_not_interrupt_new_generation() -> None:
    with TestClient(create_app()) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "session.start", "session_id": 51, "generation": 1})
            assert socket.receive_json()["type"] == "session.started"
            socket.send_json({"type": "session.cancel", "session_id": 51, "generation": 1})
            assert socket.receive_json()["type"] == "session.cancelled"

            socket.send_json({"type": "session.start", "session_id": 52, "generation": 2})
            assert socket.receive_json()["type"] == "session.started"
            codec = OpusCodec()
            packet = codec.encode(bytes(codec.frame_samples * 2))
            socket.send_bytes(AudioFrame(AudioKind.INPUT_OPUS, 51, 1, 1, 0, packet).encode())
            socket.send_json({"type": "input.commit", "session_id": 51, "generation": 1})
            socket.send_bytes(AudioFrame(AudioKind.INPUT_OPUS, 52, 2, 0, 0, packet).encode())
            socket.send_json({"type": "input.commit", "session_id": 52, "generation": 2})

            while True:
                message = socket.receive()
                if message.get("text") is None:
                    continue
                event = json.loads(message["text"])
                if event["type"] == "session.ended":
                    assert (event["session_id"], event["generation"], event["status"]) == (52, 2, "ok")
                    break


def test_empty_asr_snapshot_ends_without_llm_or_tts() -> None:
    dependencies = Dependencies(MockASRAdapter("。  "), FailIfCalledLLMAdapter(), MockTTSAdapter())
    with TestClient(create_app(dependencies)) as client:
        with client.websocket_connect(
            "/v1/voice/ws", headers={"Authorization": "Bearer development-token"}
        ) as socket:
            socket.send_json(HELLO)
            assert socket.receive_json()["type"] == "ready"
            socket.send_json({"type": "session.start", "session_id": 61, "generation": 3})
            assert socket.receive_json()["type"] == "session.started"
            codec = OpusCodec()
            packet = codec.encode(bytes(codec.frame_samples * 2))
            socket.send_bytes(AudioFrame(AudioKind.INPUT_OPUS, 61, 3, 0, 0, packet).encode())
            socket.send_json({"type": "input.commit", "session_id": 61, "generation": 3})

            event_types: list[str] = []
            while True:
                event = socket.receive_json()
                event_types.append(event["type"])
                if event["type"] == "session.ended":
                    assert event["status"] == "no_speech"
                    break
            assert "transcript.snapshot" in event_types
            assert "transcript.final" in event_types
            assert "response.audio.start" not in event_types
            assert "response.text.delta" not in event_types
