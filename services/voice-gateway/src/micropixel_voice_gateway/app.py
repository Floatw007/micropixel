from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import logging
import os
import secrets
import time
import unicodedata
from collections.abc import AsyncIterator
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, status

from .adapters import (
    ASRAdapter,
    ASRSession,
    LLMAdapter,
    MockASRAdapter,
    MockLLMAdapter,
    MockTTSAdapter,
    TTSAdapter,
)
from .codec import CodecUnavailable, OpusCodec
from .protocol import (
    FRAME_DURATION_MS,
    PROTOCOL_VERSION,
    AudioFrame,
    AudioKind,
    ProtocolError,
    decode_event,
    encode_event,
    session_identity,
    validate_hello,
)

LOGGER = logging.getLogger("micropixel.voice")
_SESSION_INPUT_CAPACITY = 64


def _has_meaningful_transcript(text: str) -> bool:
    return any(unicodedata.category(character)[0] in {"L", "N"} for character in text)


@dataclasses.dataclass(slots=True)
class Dependencies:
    asr: ASRAdapter
    llm: LLMAdapter
    tts: TTSAdapter


@dataclasses.dataclass(frozen=True, slots=True)
class CommitFrame:
    """Gracefully finish queued input before starting the response pipeline."""


@dataclasses.dataclass(frozen=True, slots=True)
class CancelFrame:
    """Immediately stop a session without waiting behind queued media."""

    reason: str = "client_request"


InputFrame = AudioFrame | CommitFrame


def default_dependencies() -> Dependencies:
    backend = os.getenv("VOICE_BACKEND", "mock")
    if backend == "mock":
        return Dependencies(
            MockASRAdapter(),
            MockLLMAdapter(),
            MockTTSAdapter(
                tone_hz=int(os.getenv("VOICE_MOCK_TONE_HZ", "0")),
                minimum_audio_ms=int(os.getenv("VOICE_MOCK_AUDIO_MS", "0")),
            ),
        )
    if backend == "volcengine":
        from .volcengine import VolcASRAdapter, VolcLLMAdapter, VolcTTSAdapter

        return Dependencies(VolcASRAdapter.from_env(), VolcLLMAdapter.from_env(), VolcTTSAdapter.from_env())
    raise RuntimeError("VOICE_BACKEND must be 'mock' or 'volcengine'")


def create_app(dependencies: Dependencies | None = None) -> FastAPI:
    app = FastAPI(title="MicroPixel Voice Gateway", version="0.2.0")
    deps = dependencies or default_dependencies()
    device_token = os.getenv("VOICE_DEVICE_TOKEN", "").strip()
    if not device_token:
        raise RuntimeError("VOICE_DEVICE_TOKEN is required")

    @app.get("/healthz")
    async def healthz() -> dict[str, str]:
        return {"status": "ok"}

    @app.websocket("/v1/voice/ws")
    async def voice_socket(websocket: WebSocket) -> None:
        authorization = websocket.headers.get("authorization", "")
        supplied = authorization.removeprefix("Bearer ") if authorization.startswith("Bearer ") else ""
        if not supplied or not secrets.compare_digest(supplied, device_token):
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return
        await websocket.accept()
        try:
            await VoiceConnection(websocket, deps).run()
        except WebSocketDisconnect:
            return
        except ProtocolError as error:
            with contextlib.suppress(RuntimeError, WebSocketDisconnect):
                await websocket.send_text(encode_event("error", code="protocol_error", message=str(error)))
                await websocket.close(code=status.WS_1002_PROTOCOL_ERROR)
        except CodecUnavailable as error:
            with contextlib.suppress(RuntimeError, WebSocketDisconnect):
                await websocket.send_text(encode_event("error", code="codec_unavailable", message=str(error)))
                await websocket.close(code=status.WS_1011_INTERNAL_ERROR)

    return app


class VoiceConnection:
    def __init__(self, websocket: WebSocket, dependencies: Dependencies) -> None:
        self.websocket = websocket
        self.dependencies = dependencies
        self.turns: list[tuple[str, str]] = []
        self.active: SessionContext | None = None
        self._send_lock = asyncio.Lock()

    async def run(self) -> None:
        first = await asyncio.wait_for(self.websocket.receive_text(), timeout=5.0)
        hello = decode_event(first)
        validate_hello(hello)
        await self.send_event("ready", protocol=PROTOCOL_VERSION, session_timeout_s=60)
        try:
            while True:
                try:
                    message = await asyncio.wait_for(self.websocket.receive(), timeout=65.0)
                except TimeoutError as error:
                    raise ProtocolError("voice connection timed out") from error
                if message.get("type") == "websocket.disconnect":
                    raise WebSocketDisconnect()
                if message.get("bytes") is not None:
                    self._handle_audio(AudioFrame.decode(message["bytes"]))
                else:
                    await self._handle_event(decode_event(message.get("text") or ""))
        finally:
            session = self.active
            if session is not None:
                session.request_cancel(CancelFrame("connection_closed"))
                await session.wait_closed()

    async def send_event(self, event_type: str, *, session: SessionContext | None = None, **fields: Any) -> None:
        if session is not None:
            fields = {"session_id": session.session_id, "generation": session.generation, **fields}
        async with self._send_lock:
            await self.websocket.send_text(encode_event(event_type, **fields))

    async def send_audio(self, session: SessionContext, frame: AudioFrame) -> None:
        if self.active is not session or session.cancelled:
            return
        async with self._send_lock:
            if self.active is session and not session.cancelled:
                await self.websocket.send_bytes(frame.encode())

    def release(self, session: SessionContext) -> None:
        if self.active is session:
            self.active = None

    def _handle_audio(self, frame: AudioFrame) -> None:
        session = self.active
        if frame.kind != AudioKind.INPUT_OPUS:
            raise ProtocolError("client sent a non-input audio frame")
        if session is None or not session.matches(frame.session_id, frame.generation):
            LOGGER.debug(
                "discarding stale input audio: session=%d generation=%d",
                frame.session_id,
                frame.generation,
            )
            return
        if session.cancelled:
            return
        session.enqueue(frame)

    async def _handle_event(self, event: dict[str, Any]) -> None:
        event_type = event["type"]
        if event_type == "ping":
            await self.send_event("pong", timestamp_us=event.get("timestamp_us", 0))
            return

        if event_type == "context.clear":
            if self.active is not None:
                raise ProtocolError("context cannot be cleared during an active session")
            self.turns.clear()
            await self.send_event("context.cleared")
            return

        session_id, generation = session_identity(event)
        if event_type == "session.start":
            if self.active is not None:
                raise ProtocolError("session is already active")
            session = SessionContext(self, session_id, generation)
            self.active = session
            session.start()
            return

        session = self.active
        if event_type == "session.cancel":
            if session is None or not session.matches(session_id, generation):
                await self.send_event(
                    "session.cancelled", session_id=session_id, generation=generation, stale=True
                )
                return
            session.request_cancel(CancelFrame())
            return

        if session is None:
            raise ProtocolError("session event received without an active session")
        if not session.matches(session_id, generation):
            LOGGER.debug(
                "discarding stale session event: type=%s session=%d generation=%d",
                event_type,
                session_id,
                generation,
            )
            return
        if event_type == "input.commit":
            session.commit()
        else:
            raise ProtocolError(f"unsupported event type: {event_type}")


class SessionContext:
    def __init__(self, connection: VoiceConnection, session_id: int, generation: int) -> None:
        self.connection = connection
        self.session_id = session_id
        self.generation = generation
        self.input: asyncio.Queue[InputFrame] = asyncio.Queue(maxsize=_SESSION_INPUT_CAPACITY)
        self.asr: ASRSession | None = None
        self.codec: OpusCodec | None = None
        self.expected_sequence: int | None = None
        self.cancel_frame: CancelFrame | None = None
        self.committed = False
        self.task: asyncio.Task[None] | None = None
        self.cancel_ack_task: asyncio.Task[None] | None = None
        self.transcript_snapshot = ""

    @property
    def cancelled(self) -> bool:
        return self.cancel_frame is not None

    def matches(self, session_id: int, generation: int) -> bool:
        return self.session_id == session_id and self.generation == generation

    def require_identity(self, session_id: int, generation: int) -> None:
        if not self.matches(session_id, generation):
            raise ProtocolError("stale or mismatched session identity")

    def start(self) -> None:
        self.task = asyncio.create_task(self._run(), name=f"voice-session-{self.session_id}-{self.generation}")

    def enqueue(self, frame: AudioFrame) -> None:
        if self.cancelled or self.committed:
            raise ProtocolError("audio received after session input closed")
        if self.expected_sequence is not None and frame.sequence != self.expected_sequence:
            raise ProtocolError("non-contiguous input audio sequence")
        self.expected_sequence = (frame.sequence + 1) & 0xFFFFFFFF
        try:
            self.input.put_nowait(frame)
        except asyncio.QueueFull as error:
            raise ProtocolError("session input queue overflow") from error

    def commit(self) -> None:
        if self.cancelled or self.committed:
            raise ProtocolError("session input is already closed")
        self.committed = True
        try:
            self.input.put_nowait(CommitFrame())
        except asyncio.QueueFull as error:
            raise ProtocolError("session input queue overflow at commit") from error

    def request_cancel(self, frame: CancelFrame) -> None:
        if self.cancel_frame is not None:
            return
        self.cancel_frame = frame
        if self.task is not None and not self.task.done():
            self.task.cancel()
        self.cancel_ack_task = asyncio.create_task(
            self._acknowledge_cancel(), name=f"voice-cancel-{self.session_id}-{self.generation}"
        )

    async def wait_closed(self) -> None:
        waiter = self.cancel_ack_task or self.task
        if waiter is not None:
            with contextlib.suppress(asyncio.CancelledError):
                await waiter

    async def _acknowledge_cancel(self) -> None:
        if self.task is not None:
            with contextlib.suppress(asyncio.CancelledError):
                await self.task
        if self.asr is not None:
            with contextlib.suppress(Exception, asyncio.CancelledError):
                await self.asr.close()
            self.asr = None
        self.connection.release(self)
        with contextlib.suppress(RuntimeError, WebSocketDisconnect):
            await self.connection.send_event(
                "session.cancelled", session=self, reason=(self.cancel_frame or CancelFrame()).reason
            )

    async def _run(self) -> None:
        cancelled = False
        error: Exception | None = None
        try:
            self.asr = await self.connection.dependencies.asr.open()
            self.codec = OpusCodec()
            await self.connection.send_event("session.started", session=self)
            await self._consume_input()
        except asyncio.CancelledError:
            cancelled = True
        except Exception as caught:
            error = caught
        finally:
            if self.asr is not None:
                with contextlib.suppress(Exception, asyncio.CancelledError):
                    await self.asr.close()
                self.asr = None
            self.connection.release(self)

        if cancelled or self.cancelled:
            return
        if error is not None:
            LOGGER.warning("voice provider session failed: %s", error)
            with contextlib.suppress(RuntimeError, WebSocketDisconnect):
                await self.connection.send_event("error", session=self, code="provider_error", message=str(error))
                await self.connection.send_event("session.ended", session=self, status="error")

    async def _consume_input(self) -> None:
        assert self.asr is not None and self.codec is not None
        while True:
            frame = await self.input.get()
            if isinstance(frame, CommitFrame):
                break
            await self.asr.write(self.codec.decode(frame.payload))
            for transcript in self.asr.drain_updates():
                self.transcript_snapshot = transcript.text
                await self.connection.send_event("transcript.snapshot", session=self, text=transcript.text)

        async for transcript in self.asr.finish():
            self.transcript_snapshot = transcript.text
            if not transcript.final:
                await self.connection.send_event("transcript.snapshot", session=self, text=transcript.text)
        await self.asr.close()
        self.asr = None
        prompt = self.transcript_snapshot.strip()
        await self.connection.send_event("transcript.final", session=self, text=prompt)
        if not _has_meaningful_transcript(prompt):
            await self.connection.send_event("session.ended", session=self, status="no_speech")
            return
        answer = await _respond(self, tuple(self.connection.turns[-16:]), prompt)
        self.connection.turns.extend((("user", prompt), ("assistant", answer)))
        await self.connection.send_event("session.ended", session=self, status="ok")


async def _respond(session: SessionContext, turns: tuple[tuple[str, str], ...], prompt: str) -> str:
    connection = session.connection
    deps = connection.dependencies
    started = time.monotonic()
    answer_parts: list[str] = []

    async def llm_text() -> AsyncIterator[str]:
        async for delta in deps.llm.stream(turns, prompt):
            answer_parts.append(delta)
            await connection.send_event("response.text.delta", session=session, text=delta)
            yield delta
        await connection.send_event("response.text.final", session=session)

    await connection.send_event("response.audio.start", session=session)
    codec = OpusCodec()
    sequence = 0
    pcm_pending = bytearray()
    next_audio_send_at: float | None = None

    async def send_audio_frame(pcm: bytes) -> None:
        nonlocal next_audio_send_at, sequence
        frame_seconds = FRAME_DURATION_MS / 1_000
        now = time.monotonic()
        if next_audio_send_at is None:
            next_audio_send_at = now
        elif now < next_audio_send_at:
            await asyncio.sleep(next_audio_send_at - now)
        elif now - next_audio_send_at > frame_seconds:
            next_audio_send_at = now
        packet = codec.encode(pcm)
        timestamp_us = int((time.monotonic() - started) * 1_000_000)
        await connection.send_audio(
            session,
            AudioFrame(
                AudioKind.OUTPUT_OPUS,
                session.session_id,
                session.generation,
                sequence,
                timestamp_us,
                packet,
            ),
        )
        next_audio_send_at += frame_seconds
        sequence = (sequence + 1) & 0xFFFFFFFF

    async for pcm in deps.tts.stream(llm_text()):
        pcm_pending.extend(pcm)
        frame_bytes = codec.frame_samples * 2
        while len(pcm_pending) >= frame_bytes:
            chunk = bytes(pcm_pending[:frame_bytes])
            del pcm_pending[:frame_bytes]
            await send_audio_frame(chunk)
    if pcm_pending:
        pcm_pending.extend(b"\0" * (codec.frame_samples * 2 - len(pcm_pending)))
        await send_audio_frame(bytes(pcm_pending))
    await connection.send_event(
        "response.audio.stop", session=session, elapsed_ms=int((time.monotonic() - started) * 1000)
    )
    return "".join(answer_parts)


app = create_app()
