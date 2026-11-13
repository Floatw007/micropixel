from __future__ import annotations

import argparse
import asyncio
import dataclasses
import json
import os
import secrets
import time
import wave
from pathlib import Path
from typing import Any

from websockets.asyncio.client import connect

from .codec import OpusCodec
from .protocol import AudioFrame, AudioKind, CHANNELS, FRAME_DURATION_MS, PROTOCOL_VERSION, SAMPLE_RATE


@dataclasses.dataclass(frozen=True, slots=True)
class FixtureResult:
    status: str
    input_frames: int
    transcript: str
    response: str
    output_pcm16: bytes
    output_frames: int
    first_transcript_ms: int | None
    first_text_ms: int | None
    first_audio_ms: int | None
    total_ms: int
    events: tuple[dict[str, Any], ...]


def read_pcm16_wave(path: Path) -> bytes:
    with wave.open(str(path), "rb") as source:
        properties = (source.getframerate(), source.getnchannels(), source.getsampwidth(), source.getcomptype())
        expected = (SAMPLE_RATE, CHANNELS, 2, "NONE")
        if properties != expected:
            raise ValueError(
                f"input WAV must be {SAMPLE_RATE} Hz, {CHANNELS} channel, 16-bit PCM; got "
                f"{properties[0]} Hz, {properties[1]} channel(s), {properties[2] * 8}-bit, {properties[3]}"
            )
        return source.readframes(source.getnframes())


def write_pcm16_wave(path: Path, pcm16: bytes) -> None:
    if len(pcm16) % 2 != 0:
        raise ValueError("PCM16 payload must contain complete samples")
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as destination:
        destination.setnchannels(CHANNELS)
        destination.setsampwidth(2)
        destination.setframerate(SAMPLE_RATE)
        destination.writeframes(pcm16)


async def run_fixture(
    url: str,
    token: str,
    pcm16: bytes,
    *,
    realtime: bool = True,
    response_timeout_s: float = 45.0,
) -> FixtureResult:
    if not token:
        raise ValueError("device bearer token is empty")
    codec = OpusCodec()
    frame_bytes = codec.frame_samples * 2
    if not pcm16:
        raise ValueError("input WAV contains no audio")
    if len(pcm16) % 2 != 0:
        raise ValueError("input PCM16 payload contains an incomplete sample")

    input_frames = (len(pcm16) + frame_bytes - 1) // frame_bytes
    events: list[dict[str, Any]] = []
    transcript = ""
    response_parts: list[str] = []
    output_pcm = bytearray()
    output_frames = 0
    expected_output_sequence: int | None = None
    first_transcript_ms: int | None = None
    first_text_ms: int | None = None
    first_audio_ms: int | None = None
    session_id = secrets.randbits(32) or 1
    generation = secrets.randbits(32) or 1

    headers = {"Authorization": f"Bearer {token}"}
    async with connect(url, additional_headers=headers, max_size=1_000_000, open_timeout=10) as socket:
        await socket.send(
            json.dumps(
                {
                    "type": "hello",
                    "protocol": PROTOCOL_VERSION,
                    "codec": "opus",
                    "sample_rate": SAMPLE_RATE,
                    "channels": CHANNELS,
                    "frame_duration_ms": FRAME_DURATION_MS,
                },
                separators=(",", ":"),
            )
        )
        ready = json.loads(await asyncio.wait_for(socket.recv(), timeout=5))
        if ready.get("type") != "ready":
            raise RuntimeError(f"gateway did not accept protocol negotiation: {ready.get('type')}")
        await socket.send(
            json.dumps(
                {"type": "session.start", "session_id": session_id, "generation": generation},
                separators=(",", ":"),
            )
        )
        started = json.loads(await asyncio.wait_for(socket.recv(), timeout=10))
        if (
            started.get("type") != "session.started"
            or started.get("session_id") != session_id
            or started.get("generation") != generation
        ):
            raise RuntimeError(f"gateway did not start the session: {started.get('type')}")

        upload_started = time.monotonic()
        committed: float | None = None

        async def receive() -> FixtureResult:
            nonlocal expected_output_sequence, first_audio_ms, first_text_ms, first_transcript_ms, output_frames
            nonlocal transcript
            while True:
                message = await socket.recv()
                now = time.monotonic()
                response_elapsed_ms = int((now - (committed or upload_started)) * 1_000)
                if isinstance(message, bytes):
                    frame = AudioFrame.decode(message)
                    if frame.kind != AudioKind.OUTPUT_OPUS:
                        raise RuntimeError(f"unexpected downlink audio kind: {frame.kind}")
                    if frame.session_id != session_id or frame.generation != generation:
                        raise RuntimeError("gateway returned audio for a stale session generation")
                    if expected_output_sequence is not None and frame.sequence != expected_output_sequence:
                        raise RuntimeError(
                            f"non-contiguous downlink sequence: expected {expected_output_sequence}, got {frame.sequence}"
                        )
                    expected_output_sequence = (frame.sequence + 1) & 0xFFFFFFFF
                    output_pcm.extend(codec.decode(frame.payload))
                    output_frames += 1
                    if first_audio_ms is None:
                        first_audio_ms = response_elapsed_ms
                    continue

                event = json.loads(message)
                if not isinstance(event, dict) or not isinstance(event.get("type"), str):
                    raise RuntimeError("gateway returned an invalid control event")
                events.append(event)
                event_type = event["type"]
                if event_type not in {"ready", "pong"} and (
                    event.get("session_id") != session_id or event.get("generation") != generation
                ):
                    raise RuntimeError("gateway returned an event for a stale session generation")
                if event_type in {"transcript.snapshot", "transcript.final"}:
                    transcript = str(event.get("text", transcript))
                    if first_transcript_ms is None:
                        first_transcript_ms = int((now - upload_started) * 1_000)
                elif event_type == "response.text.delta":
                    response_parts.append(str(event.get("text", "")))
                    if first_text_ms is None:
                        first_text_ms = response_elapsed_ms
                elif event_type == "error":
                    raise RuntimeError(f"gateway error {event.get('code', 'unknown')}: {event.get('message', '')}")
                elif event_type == "response.audio.stop":
                    if not transcript:
                        raise RuntimeError("gateway completed without a transcript")
                    if not response_parts:
                        raise RuntimeError("gateway completed without response text")
                    if not output_pcm:
                        raise RuntimeError("gateway completed without response audio")
                    return FixtureResult(
                        status="ok",
                        input_frames=input_frames,
                        transcript=transcript,
                        response="".join(response_parts),
                        output_pcm16=bytes(output_pcm),
                        output_frames=output_frames,
                        first_transcript_ms=first_transcript_ms,
                        first_text_ms=first_text_ms,
                        first_audio_ms=first_audio_ms,
                        total_ms=response_elapsed_ms,
                        events=tuple(events),
                    )
                elif event_type == "session.ended" and event.get("status") == "no_speech":
                    return FixtureResult(
                        status="no_speech",
                        input_frames=input_frames,
                        transcript=transcript,
                        response="",
                        output_pcm16=b"",
                        output_frames=0,
                        first_transcript_ms=first_transcript_ms,
                        first_text_ms=None,
                        first_audio_ms=None,
                        total_ms=response_elapsed_ms,
                        events=tuple(events),
                    )

        receiver = asyncio.create_task(receive(), name="voice-fixture-receive")
        try:
            for sequence in range(input_frames):
                offset = sequence * frame_bytes
                chunk = pcm16[offset : offset + frame_bytes]
                if len(chunk) < frame_bytes:
                    chunk += b"\0" * (frame_bytes - len(chunk))
                if realtime and sequence != 0:
                    target = upload_started + sequence * FRAME_DURATION_MS / 1_000
                    delay = target - time.monotonic()
                    if delay > 0:
                        await asyncio.sleep(delay)
                await socket.send(
                    AudioFrame(
                        AudioKind.INPUT_OPUS,
                        session_id,
                        generation,
                        sequence,
                        sequence * FRAME_DURATION_MS * 1_000,
                        codec.encode(chunk),
                    ).encode()
                )
            committed = time.monotonic()
            await socket.send(
                json.dumps(
                    {"type": "input.commit", "session_id": session_id, "generation": generation},
                    separators=(",", ":"),
                )
            )
            return await asyncio.wait_for(receiver, timeout=response_timeout_s)
        finally:
            if not receiver.done():
                receiver.cancel()
                try:
                    await receiver
                except asyncio.CancelledError:
                    pass


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inject a prerecorded WAV into the Bear voice protocol")
    parser.add_argument("--input-wav", type=Path, required=True)
    parser.add_argument("--output-wav", type=Path, required=True)
    parser.add_argument("--events-json", type=Path)
    parser.add_argument("--url", default=os.getenv("VOICE_GATEWAY_URL", "ws://127.0.0.1:8080/v1/voice/ws"))
    parser.add_argument("--fast", action="store_true", help="Upload without real-time 20 ms pacing")
    parser.add_argument("--show-text", action="store_true", help="Print transcript and response text")
    parser.add_argument("--timeout", type=float, default=45.0)
    return parser


def main() -> None:
    args = _parser().parse_args()
    token = os.getenv("VOICE_DEVICE_TOKEN", "")
    if not token:
        raise SystemExit("VOICE_DEVICE_TOKEN is required in the environment")
    pcm16 = read_pcm16_wave(args.input_wav)
    result = asyncio.run(
        run_fixture(args.url, token, pcm16, realtime=not args.fast, response_timeout_s=args.timeout)
    )
    write_pcm16_wave(args.output_wav, result.output_pcm16)
    if args.events_json is not None:
        args.events_json.parent.mkdir(parents=True, exist_ok=True)
        args.events_json.write_text(
            json.dumps(list(result.events), ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    summary = {
        "status": result.status,
        "input_frames": result.input_frames,
        "transcript_characters": len(result.transcript),
        "response_characters": len(result.response),
        "output_frames": result.output_frames,
        "output_pcm_bytes": len(result.output_pcm16),
        "first_transcript_from_upload_ms": result.first_transcript_ms,
        "first_text_from_commit_ms": result.first_text_ms,
        "first_audio_from_commit_ms": result.first_audio_ms,
        "response_total_ms": result.total_ms,
        "output_wav": str(args.output_wav),
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if args.show_text:
        print(f"transcript: {result.transcript}")
        print(f"response: {result.response}")


if __name__ == "__main__":
    main()
