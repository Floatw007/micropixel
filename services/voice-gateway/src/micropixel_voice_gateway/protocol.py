from __future__ import annotations

import dataclasses
import enum
import json
import struct
from typing import Any

PROTOCOL_VERSION = 2
SAMPLE_RATE = 16_000
CHANNELS = 1
FRAME_DURATION_MS = 20
MAX_AUDIO_PAYLOAD = 1_500
MAX_JSON_BYTES = 8_192

_AUDIO_HEADER = struct.Struct("<BBBBIIIQI")


class ProtocolError(ValueError):
    pass


class AudioKind(enum.IntEnum):
    INPUT_OPUS = 1
    OUTPUT_OPUS = 2


@dataclasses.dataclass(frozen=True, slots=True)
class AudioFrame:
    kind: AudioKind
    session_id: int
    generation: int
    sequence: int
    timestamp_us: int
    payload: bytes
    flags: int = 0

    def encode(self) -> bytes:
        if not self.payload or len(self.payload) > MAX_AUDIO_PAYLOAD:
            raise ProtocolError("invalid audio payload length")
        if not 1 <= self.session_id <= 0xFFFFFFFF or not 1 <= self.generation <= 0xFFFFFFFF:
            raise ProtocolError("audio session identity is out of range")
        if not 0 <= self.sequence <= 0xFFFFFFFF or not 0 <= self.timestamp_us <= 0xFFFFFFFFFFFFFFFF:
            raise ProtocolError("audio sequence or timestamp is out of range")
        return _AUDIO_HEADER.pack(
            PROTOCOL_VERSION,
            int(self.kind),
            self.flags & 0xFF,
            0,
            self.session_id,
            self.generation,
            self.sequence,
            self.timestamp_us,
            len(self.payload),
        ) + self.payload

    @classmethod
    def decode(cls, wire: bytes) -> "AudioFrame":
        if len(wire) < _AUDIO_HEADER.size:
            raise ProtocolError("truncated audio header")
        version, raw_kind, flags, reserved, session_id, generation, sequence, timestamp_us, length = (
            _AUDIO_HEADER.unpack_from(wire)
        )
        if version != PROTOCOL_VERSION or flags != 0 or reserved != 0:
            raise ProtocolError("unsupported audio envelope")
        try:
            kind = AudioKind(raw_kind)
        except ValueError as error:
            raise ProtocolError("unknown audio kind") from error
        if length == 0 or length > MAX_AUDIO_PAYLOAD or len(wire) != _AUDIO_HEADER.size + length:
            raise ProtocolError("invalid audio payload length")
        if session_id == 0 or generation == 0:
            raise ProtocolError("audio session identity is missing")
        return cls(
            kind=kind,
            session_id=session_id,
            generation=generation,
            sequence=sequence,
            timestamp_us=timestamp_us,
            payload=wire[_AUDIO_HEADER.size :],
            flags=flags,
        )


def encode_event(event_type: str, **fields: Any) -> str:
    return json.dumps({"type": event_type, **fields}, ensure_ascii=False, separators=(",", ":"))


def decode_event(wire: str) -> dict[str, Any]:
    if len(wire.encode("utf-8")) > MAX_JSON_BYTES:
        raise ProtocolError("control message is too large")
    try:
        event = json.loads(wire)
    except json.JSONDecodeError as error:
        raise ProtocolError("invalid JSON control message") from error
    if not isinstance(event, dict) or not isinstance(event.get("type"), str):
        raise ProtocolError("control message requires a string type")
    return event


def validate_hello(event: dict[str, Any]) -> None:
    expected = {
        "protocol": PROTOCOL_VERSION,
        "codec": "opus",
        "sample_rate": SAMPLE_RATE,
        "channels": CHANNELS,
        "frame_duration_ms": FRAME_DURATION_MS,
    }
    if event.get("type") != "hello" or any(event.get(key) != value for key, value in expected.items()):
        raise ProtocolError("unsupported voice negotiation")


def session_identity(event: dict[str, Any]) -> tuple[int, int]:
    session_id = event.get("session_id")
    generation = event.get("generation")
    if (
        not isinstance(session_id, int)
        or isinstance(session_id, bool)
        or not 1 <= session_id <= 0xFFFFFFFF
        or not isinstance(generation, int)
        or isinstance(generation, bool)
        or not 1 <= generation <= 0xFFFFFFFF
    ):
        raise ProtocolError("session event requires non-zero uint32 session_id and generation")
    return session_id, generation
