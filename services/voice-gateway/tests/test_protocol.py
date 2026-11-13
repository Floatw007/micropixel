from pathlib import Path

import pytest

from micropixel_voice_gateway.chunker import sentence_chunks
from micropixel_voice_gateway.fixture_client import read_pcm16_wave, write_pcm16_wave
from micropixel_voice_gateway.protocol import AudioFrame, AudioKind, ProtocolError, session_identity, validate_hello
from micropixel_voice_gateway.volcengine import _connection_failure, _encode_asr_request, _encode_tts_event


def test_audio_frame_round_trip() -> None:
    frame = AudioFrame(AudioKind.INPUT_OPUS, 11, 3, 7, 123456, b"opus")
    assert AudioFrame.decode(frame.encode()) == frame


def test_audio_frame_rejects_trailing_data() -> None:
    wire = AudioFrame(AudioKind.INPUT_OPUS, 11, 3, 0, 0, b"x").encode() + b"y"
    with pytest.raises(ProtocolError):
        AudioFrame.decode(wire)


def test_hello_is_strict() -> None:
    with pytest.raises(ProtocolError):
        validate_hello({"type": "hello", "protocol": 1, "codec": "pcm"})


def test_session_identity_is_strict() -> None:
    assert session_identity({"session_id": 7, "generation": 9}) == (7, 9)
    for event in ({}, {"session_id": 0, "generation": 1}, {"session_id": 1, "generation": True}):
        with pytest.raises(ProtocolError):
            session_identity(event)


@pytest.mark.asyncio
async def test_sentence_chunking() -> None:
    async def source():
        for value in ("你好", "。继续", "说完"):
            yield value

    assert [value async for value in sentence_chunks(source())] == ["你好。", "继续说完"]


def test_volc_asr_audio_last_packet() -> None:
    wire = _encode_asr_request(b"pcm", audio=True, final=True)
    assert wire[:4] == bytes((0x11, 0x22, 0x00, 0x00))
    assert wire[4:8] == bytes((0, 0, 0, 3))
    assert wire[8:] == b"pcm"


def test_volc_tts_task_event_layout() -> None:
    wire = _encode_tts_event(200, {"req_params": {"text": "你好"}}, "session-1")
    assert wire[:4] == bytes((0x11, 0x14, 0x10, 0x00))
    assert int.from_bytes(wire[4:8], "big", signed=True) == 200
    assert int.from_bytes(wire[8:12], "big") == len("session-1")


def test_volc_handshake_error_reports_resource_without_secrets() -> None:
    class Response:
        status_code = 403
        body = bytearray(
            b'{"error":"[resource_id=volc.seedasr.sauc.duration] requested resource not granted"}'
        )

    class Failure(Exception):
        response = Response()

    message = str(_connection_failure("ASR", Failure("must-not-be-copied")))
    assert message == (
        "ASR connection rejected with HTTP 403: "
        "resource volc.seedasr.sauc.duration is not granted"
    )
    assert "must-not-be-copied" not in message


def test_fixture_wave_round_trip(tmp_path: Path) -> None:
    path = tmp_path / "fixture.wav"
    pcm16 = bytes(range(128)) * 10
    write_pcm16_wave(path, pcm16)
    assert read_pcm16_wave(path) == pcm16


def test_fixture_rejects_wrong_sample_rate(tmp_path: Path) -> None:
    import wave

    path = tmp_path / "wrong-rate.wav"
    with wave.open(str(path), "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(8_000)
        destination.writeframes(b"\0\0" * 80)
    with pytest.raises(ValueError, match="16000 Hz"):
        read_pcm16_wave(path)
