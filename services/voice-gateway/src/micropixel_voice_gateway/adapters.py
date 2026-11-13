from __future__ import annotations

import abc
import asyncio
import dataclasses
import math
import struct
from collections.abc import AsyncIterator


class AdapterError(RuntimeError):
    """Provider failure safe to translate into a device-facing error."""


@dataclasses.dataclass(frozen=True, slots=True)
class Transcript:
    text: str
    final: bool


class ASRSession(abc.ABC):
    @abc.abstractmethod
    async def write(self, pcm16: bytes) -> None: ...

    def drain_updates(self) -> tuple[Transcript, ...]:
        return ()

    @abc.abstractmethod
    async def finish(self) -> AsyncIterator[Transcript]: ...

    @abc.abstractmethod
    async def close(self) -> None: ...


class ASRAdapter(abc.ABC):
    @abc.abstractmethod
    async def open(self) -> ASRSession: ...


class LLMAdapter(abc.ABC):
    @abc.abstractmethod
    async def stream(self, turns: tuple[tuple[str, str], ...], prompt: str) -> AsyncIterator[str]: ...


class TTSAdapter(abc.ABC):
    @abc.abstractmethod
    async def stream(self, text: AsyncIterator[str]) -> AsyncIterator[bytes]:
        """Yield signed 16-bit, 16 kHz, mono PCM."""


class _MockASRSession(ASRSession):
    def __init__(self, transcript: str) -> None:
        self._transcript = transcript
        self._received = 0

    async def write(self, pcm16: bytes) -> None:
        self._received += len(pcm16)

    async def finish(self) -> AsyncIterator[Transcript]:
        if self._received:
            midpoint = max(1, len(self._transcript) // 2)
            yield Transcript(self._transcript[:midpoint], False)
            await asyncio.sleep(0)
        yield Transcript(self._transcript, True)

    async def close(self) -> None:
        self._received = 0


class MockASRAdapter(ASRAdapter):
    def __init__(self, transcript: str = "你好 Bear") -> None:
        self._transcript = transcript

    async def open(self) -> ASRSession:
        return _MockASRSession(self._transcript)


class MockLLMAdapter(LLMAdapter):
    async def stream(self, turns: tuple[tuple[str, str], ...], prompt: str) -> AsyncIterator[str]:
        del turns
        for chunk in ("我听到了：", prompt, "。很高兴和你聊天！"):
            yield chunk
            await asyncio.sleep(0)


class MockTTSAdapter(TTSAdapter):
    def __init__(self, tone_hz: int = 0, minimum_audio_ms: int = 0) -> None:
        self._tone_hz = max(0, tone_hz)
        self._minimum_frames = max(0, (minimum_audio_ms + 19) // 20)

    async def stream(self, text: AsyncIterator[str]) -> AsyncIterator[bytes]:
        # Silence remains the default deterministic backend. A low-amplitude test
        # tone can be enabled explicitly to validate a real device speaker path.
        phase = 0
        emitted_frames = 0

        def frame() -> bytes:
            nonlocal phase
            if self._tone_hz == 0:
                return bytes(320 * 2)
            samples = []
            for _ in range(320):
                sample = round(4096 * math.sin(2 * math.pi * self._tone_hz * phase / 16000))
                samples.append(sample)
                phase += 1
            return struct.pack("<320h", *samples)

        async for chunk in text:
            frames = max(1, len(chunk.encode("utf-8")) // 3)
            for _ in range(frames):
                yield frame()
                emitted_frames += 1
                await asyncio.sleep(0.02 if self._tone_hz else 0)
        while emitted_frames < self._minimum_frames:
            yield frame()
            emitted_frames += 1
            await asyncio.sleep(0.02 if self._tone_hz else 0)
