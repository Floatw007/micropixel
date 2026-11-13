from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import gzip
import json
import os
import re
import struct
import uuid
from collections import deque
from collections.abc import AsyncIterator
from typing import Any

import httpx
from websockets.asyncio.client import connect

from .adapters import AdapterError, ASRAdapter, ASRSession, LLMAdapter, Transcript, TTSAdapter

ASR_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
TTS_URL = "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
ARK_BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"

_ASR_BATCH_BYTES = 16_000 * 2 * 200 // 1_000
_HEADER = struct.Struct(">BBBB")
_U32 = struct.Struct(">I")
_I32 = struct.Struct(">i")
_RESOURCE_ID = re.compile(r"\[resource_id=([A-Za-z0-9._-]+)\]")


def _required(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"missing required environment variable: {name}")
    return value


def _connection_failure(provider: str, error: Exception) -> AdapterError:
    """Return a useful handshake error without copying credentials or headers."""
    response = getattr(error, "response", None)
    status = getattr(response, "status_code", None)
    if not isinstance(status, int):
        return AdapterError(f"{provider} connection failed: {type(error).__name__}")

    body = getattr(response, "body", b"") or b""
    if isinstance(body, (bytes, bytearray)):
        body = bytes(body).decode("utf-8", errors="replace")
    resource = _RESOURCE_ID.search(str(body))
    if status == 403 and resource and "requested resource not granted" in str(body):
        return AdapterError(
            f"{provider} connection rejected with HTTP 403: resource {resource.group(1)} is not granted"
        )
    if status in {401, 403}:
        return AdapterError(f"{provider} connection rejected with HTTP {status}: check API key and resource access")
    return AdapterError(f"{provider} connection rejected with HTTP {status}")


@dataclasses.dataclass(frozen=True, slots=True)
class _Frame:
    message_type: int
    flags: int
    serialization: int
    compression: int
    payload: bytes
    event: int | None = None
    sequence: int | None = None
    identifier: str = ""
    error_code: int | None = None


def _json_bytes(value: dict[str, Any]) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def _encode_asr_request(payload: bytes, *, audio: bool, final: bool = False) -> bytes:
    message_type = 0x2 if audio else 0x1
    flags = 0x2 if final else 0x0
    serialization = 0x0 if audio else 0x1
    header = _HEADER.pack(0x11, (message_type << 4) | flags, serialization << 4, 0)
    return header + _U32.pack(len(payload)) + payload


def _encode_tts_event(event: int, payload: dict[str, Any], session_id: str = "") -> bytes:
    encoded = _json_bytes(payload)
    wire = bytearray(_HEADER.pack(0x11, 0x14, 0x10, 0))
    wire.extend(_I32.pack(event))
    if session_id:
        identifier = session_id.encode()
        wire.extend(_U32.pack(len(identifier)))
        wire.extend(identifier)
    wire.extend(_U32.pack(len(encoded)))
    wire.extend(encoded)
    return bytes(wire)


def _decode_frame(wire: bytes, *, event_protocol: bool) -> _Frame:
    if len(wire) < 8:
        raise AdapterError("provider returned a truncated binary frame")
    first, second, third, reserved = _HEADER.unpack_from(wire)
    if first >> 4 != 1 or reserved != 0:
        raise AdapterError("provider returned an unsupported protocol frame")
    header_size = (first & 0xF) * 4
    if header_size < 4 or header_size > len(wire):
        raise AdapterError("provider returned an invalid header size")
    message_type = second >> 4
    flags = second & 0xF
    serialization = third >> 4
    compression = third & 0xF
    offset = header_size
    event = None
    sequence = None
    identifier = ""
    error_code = None
    if message_type == 0xF:
        if offset + 4 > len(wire):
            raise AdapterError("provider returned a truncated error frame")
        error_code = _U32.unpack_from(wire, offset)[0]
        offset += 4
    elif event_protocol and flags == 0x4:
        if offset + 4 > len(wire):
            raise AdapterError("provider returned a truncated event frame")
        event = _I32.unpack_from(wire, offset)[0]
        offset += 4
        if event in {50, 51, 52, 150, 151, 152, 153, 350, 351, 352}:
            if offset + 4 > len(wire):
                raise AdapterError("provider returned a truncated event identifier")
            size = _U32.unpack_from(wire, offset)[0]
            offset += 4
            if offset + size > len(wire):
                raise AdapterError("provider returned an invalid event identifier")
            identifier = wire[offset : offset + size].decode(errors="replace")
            offset += size
    elif not event_protocol and flags in {0x1, 0x3}:
        if offset + 4 > len(wire):
            raise AdapterError("provider returned a truncated sequence")
        sequence = _I32.unpack_from(wire, offset)[0]
        offset += 4
    if offset + 4 > len(wire):
        raise AdapterError("provider returned a truncated payload size")
    payload_size = _U32.unpack_from(wire, offset)[0]
    offset += 4
    if offset + payload_size != len(wire):
        raise AdapterError("provider returned an invalid payload size")
    payload = wire[offset:]
    if compression == 0x1:
        try:
            payload = gzip.decompress(payload)
        except gzip.BadGzipFile as error:
            raise AdapterError("provider returned invalid gzip data") from error
    elif compression != 0x0:
        raise AdapterError("provider returned an unsupported compression mode")
    return _Frame(message_type, flags, serialization, compression, payload, event, sequence, identifier, error_code)


def _json_payload(frame: _Frame) -> dict[str, Any]:
    if not frame.payload:
        return {}
    try:
        value = json.loads(frame.payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdapterError("provider returned invalid JSON") from error
    if not isinstance(value, dict):
        raise AdapterError("provider returned a non-object JSON payload")
    return value


def _raise_provider_error(frame: _Frame) -> None:
    if frame.message_type != 0xF:
        return
    detail = _json_payload(frame)
    message = detail.get("message") if isinstance(detail.get("message"), str) else "request failed"
    raise AdapterError(f"provider error {frame.error_code}: {message}")


@dataclasses.dataclass(frozen=True, slots=True)
class VolcASRConfig:
    api_key: str
    resource_id: str
    url: str = ASR_URL


class _VolcASRSession(ASRSession):
    def __init__(self, config: VolcASRConfig) -> None:
        self._config = config
        self._socket: Any = None
        self._receiver: asyncio.Task[None] | None = None
        self._pending = bytearray()
        self._updates: deque[Transcript] = deque()
        self._last_text = ""

    async def start(self) -> None:
        request_id = str(uuid.uuid4())
        headers = {
            "X-Api-Key": self._config.api_key,
            "X-Api-Resource-Id": self._config.resource_id,
            "X-Api-Request-Id": request_id,
            "X-Api-Connect-Id": request_id,
            "X-Api-Sequence": "-1",
        }
        try:
            self._socket = await connect(
                self._config.url, additional_headers=headers, open_timeout=8, close_timeout=3, ping_interval=20
            )
            request = {
                "user": {"uid": "micropixel-bear"},
                "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
                "request": {
                    "model_name": "bigmodel",
                    "enable_nonstream": True,
                    "enable_itn": True,
                    "enable_punc": True,
                    "show_utterances": True,
                    "result_type": "full",
                    "end_window_size": 600,
                    "force_to_speech_time": 600,
                },
            }
            await self._socket.send(_encode_asr_request(_json_bytes(request), audio=False))
            self._receiver = asyncio.create_task(self._receive(), name="volc-asr-receive")
        except Exception as error:
            await self.close()
            raise _connection_failure("ASR", error) from error

    async def _receive(self) -> None:
        assert self._socket is not None
        async for wire in self._socket:
            if not isinstance(wire, bytes):
                continue
            frame = _decode_frame(wire, event_protocol=False)
            _raise_provider_error(frame)
            body = _json_payload(frame)
            result = body.get("result")
            if not isinstance(result, dict):
                continue
            text = result.get("text")
            if not isinstance(text, str) or not text or text == self._last_text:
                continue
            utterances = result.get("utterances")
            definite = isinstance(utterances, list) and bool(utterances) and all(
                isinstance(item, dict) and item.get("definite") is True for item in utterances
            )
            final = frame.flags in {0x2, 0x3} or definite
            self._last_text = text
            self._updates.append(Transcript(text, final))

    def _check_receiver(self) -> None:
        if self._receiver is not None and self._receiver.done():
            error = self._receiver.exception()
            if error is not None:
                raise AdapterError(f"ASR receive failed: {type(error).__name__}") from error

    async def write(self, pcm16: bytes) -> None:
        self._check_receiver()
        self._pending.extend(pcm16)
        while len(self._pending) >= _ASR_BATCH_BYTES:
            chunk = bytes(self._pending[:_ASR_BATCH_BYTES])
            del self._pending[:_ASR_BATCH_BYTES]
            await self._socket.send(_encode_asr_request(chunk, audio=True))

    def drain_updates(self) -> tuple[Transcript, ...]:
        values = tuple(self._updates)
        self._updates.clear()
        return values

    async def finish(self) -> AsyncIterator[Transcript]:
        self._check_receiver()
        final_audio = bytes(self._pending)
        self._pending.clear()
        await self._socket.send(_encode_asr_request(final_audio, audio=True, final=True))
        if self._receiver is not None:
            try:
                await asyncio.wait_for(self._receiver, timeout=15)
            except TimeoutError as error:
                raise AdapterError("ASR final response timed out") from error
        for update in self.drain_updates():
            yield update

    async def close(self) -> None:
        if self._receiver is not None and not self._receiver.done():
            self._receiver.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._receiver
        self._receiver = None
        if self._socket is not None:
            await self._socket.close()
        self._socket = None
        self._pending.clear()


class VolcASRAdapter(ASRAdapter):
    def __init__(self, config: VolcASRConfig) -> None:
        self._config = config

    @classmethod
    def from_env(cls) -> "VolcASRAdapter":
        return cls(VolcASRConfig(_required("VOLC_SPEECH_API_KEY"), _required("VOLC_ASR_RESOURCE_ID")))

    async def open(self) -> ASRSession:
        session = _VolcASRSession(self._config)
        await session.start()
        return session


@dataclasses.dataclass(frozen=True, slots=True)
class VolcLLMConfig:
    api_key: str
    model: str = "doubao-seed-2-0-mini-260428"
    base_url: str = ARK_BASE_URL


class VolcLLMAdapter(LLMAdapter):
    def __init__(self, config: VolcLLMConfig) -> None:
        self._config = config

    @classmethod
    def from_env(cls) -> "VolcLLMAdapter":
        return cls(
            VolcLLMConfig(
                _required("ARK_API_KEY"),
                os.getenv("DOUBAO_MODEL", "doubao-seed-2-0-mini-260428").strip(),
                os.getenv("ARK_BASE_URL", ARK_BASE_URL).rstrip("/"),
            )
        )

    async def stream(self, turns: tuple[tuple[str, str], ...], prompt: str) -> AsyncIterator[str]:
        messages = [{"role": "system", "content": "你是 Bear。回答自然、温暖、简短，适合直接朗读。"}]
        messages.extend({"role": role, "content": text} for role, text in turns if text)
        messages.append({"role": "user", "content": prompt})
        request = {
            "model": self._config.model,
            "messages": messages,
            "stream": True,
            "max_completion_tokens": 256,
            "thinking": {"type": "disabled"},
        }
        headers = {"Authorization": f"Bearer {self._config.api_key}"}
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(30, connect=8)) as client:
                async with client.stream(
                    "POST", f"{self._config.base_url}/chat/completions", headers=headers, json=request
                ) as response:
                    if response.status_code != 200:
                        raise AdapterError(f"LLM request failed with HTTP {response.status_code}")
                    async for line in response.aiter_lines():
                        if not line.startswith("data:"):
                            continue
                        data = line[5:].strip()
                        if data == "[DONE]":
                            break
                        try:
                            event = json.loads(data)
                            delta = event["choices"][0]["delta"].get("content")
                        except (json.JSONDecodeError, KeyError, IndexError, TypeError) as error:
                            raise AdapterError("LLM returned an invalid stream event") from error
                        if isinstance(delta, str) and delta:
                            yield delta
        except AdapterError:
            raise
        except Exception as error:
            raise AdapterError(f"LLM stream failed: {type(error).__name__}") from error


@dataclasses.dataclass(frozen=True, slots=True)
class VolcTTSConfig:
    api_key: str
    resource_id: str
    speaker: str
    url: str = TTS_URL


class VolcTTSAdapter(TTSAdapter):
    def __init__(self, config: VolcTTSConfig) -> None:
        self._config = config

    @classmethod
    def from_env(cls) -> "VolcTTSAdapter":
        return cls(
            VolcTTSConfig(
                _required("VOLC_SPEECH_API_KEY"),
                _required("VOLC_TTS_RESOURCE_ID"),
                _required("VOLC_TTS_SPEAKER"),
            )
        )

    async def _wait_event(self, socket: Any, expected: int) -> _Frame:
        while True:
            wire = await asyncio.wait_for(socket.recv(), timeout=10)
            if not isinstance(wire, bytes):
                continue
            frame = _decode_frame(wire, event_protocol=True)
            _raise_provider_error(frame)
            if frame.event in {51, 153}:
                body = _json_payload(frame)
                raise AdapterError(f"TTS event {frame.event} failed: {body.get('message', 'request failed')}")
            if frame.event == expected:
                return frame

    async def stream(self, text: AsyncIterator[str]) -> AsyncIterator[bytes]:
        connect_id = str(uuid.uuid4())
        session_id = str(uuid.uuid4())
        headers = {
            "X-Api-Key": self._config.api_key,
            "X-Api-Resource-Id": self._config.resource_id,
            "X-Api-Connect-Id": connect_id,
        }
        socket: Any = None
        producer: asyncio.Task[None] | None = None
        try:
            socket = await connect(
                self._config.url, additional_headers=headers, open_timeout=8, close_timeout=3, ping_interval=20
            )
            await socket.send(_encode_tts_event(1, {}))
            await self._wait_event(socket, 50)
            start = {
                "user": {"uid": "micropixel-bear"},
                "event": 100,
                "req_params": {
                    "text": "",
                    "speaker": self._config.speaker,
                    "model": "seed-tts-2.0-standard",
                    "audio_params": {"format": "pcm", "sample_rate": 16000},
                    "additions": json.dumps(
                        {"disable_markdown_filter": True, "explicit_language": "zh-cn"}, separators=(",", ":")
                    ),
                },
            }
            await socket.send(_encode_tts_event(100, start, session_id))
            await self._wait_event(socket, 150)

            async def produce() -> None:
                async for delta in text:
                    if delta:
                        await socket.send(_encode_tts_event(200, {"req_params": {"text": delta}}, session_id))
                await socket.send(_encode_tts_event(102, {}, session_id))

            producer = asyncio.create_task(produce(), name="volc-tts-produce")
            while True:
                wire = await asyncio.wait_for(socket.recv(), timeout=30)
                if not isinstance(wire, bytes):
                    continue
                frame = _decode_frame(wire, event_protocol=True)
                _raise_provider_error(frame)
                if frame.event == 352 and frame.payload:
                    yield frame.payload
                elif frame.event == 152:
                    break
                elif frame.event == 153:
                    body = _json_payload(frame)
                    raise AdapterError(f"TTS session failed: {body.get('message', 'request failed')}")
            await producer
            await socket.send(_encode_tts_event(2, {}))
            with contextlib.suppress(TimeoutError):
                await self._wait_event(socket, 52)
        except AdapterError:
            raise
        except Exception as error:
            raise _connection_failure("TTS", error) from error
        finally:
            if producer is not None and not producer.done():
                producer.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await producer
            if socket is not None:
                await socket.close()
