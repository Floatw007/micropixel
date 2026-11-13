# MicroPixel Voice Gateway

Provider-neutral, streaming WebSocket gateway for the Bear application. The
gateway never stores raw audio or transcripts. Development defaults to a mock
ASR/LLM/TTS pipeline; cloud credentials stay server-side.

## Run locally

```sh
cp .env.example .env
# Replace VOICE_DEVICE_TOKEN before starting.
docker compose up --build
curl http://127.0.0.1:8080/healthz
```

The device endpoint is `ws://HOST:8080/v1/voice/ws`. Plain `ws://` is for an
explicitly enabled private-LAN development profile only; deployments must put
the same container behind a TLS reverse proxy and use `wss://`.

## Prerecorded end-to-end test

The fixture client injects a 16 kHz, 16-bit, mono PCM WAV as real-time 20 ms
Opus frames and decodes the returned Opus stream into a WAV. This validates
ASR, LLM, TTS, protocol ordering and the downloadable audio without requiring
a live microphone or speaker. Keep generated recordings under the ignored
`artifacts/` directory.

```sh
set -a
. services/voice-gateway/.env
set +a
micropixel-voice-fixture \
  --input-wav artifacts/voice-e2e/input.wav \
  --output-wav artifacts/voice-e2e/response.wav \
  --events-json artifacts/voice-e2e/events.json
```

Use `--fast` only for load testing; default real-time pacing matches the
device. The first-transcript metric is measured from upload start; response
text/audio metrics are measured from `input.commit`. Text is omitted from
terminal output unless `--show-text` is supplied.

## Protocol v2

The client first sends `hello` with protocol `2`, codec `opus`, sample rate
`16000`, one channel, and `20` ms frames. Control messages are UTF-8 JSON.
Every session starts with client-generated non-zero `session_id` and
`generation` values. All session-scoped control messages echo both values, so
late text, audio, and terminal events from an old generation can be discarded.

Audio messages use a 28-byte little-endian header (`version`, `kind`, `flags`,
reserved, `session_id`, `generation`, sequence, timestamp in microseconds,
payload length) followed by one raw Opus packet. Input kind is `1`; output kind
is `2`.

The connection receiver remains active while ASR, LLM, and TTS run in a
separate per-session task. `session.cancel` is a priority control operation: it
cancels that task immediately, closes provider sessions, drops queued media,
and replies with `session.cancelled`. Normal audio and `input.commit` commands
use a bounded per-session queue and cannot delay cancellation.

ASR text is snapshot-based: `transcript.snapshot` replaces the device's current
transcript, and `transcript.final` supplies the final replacement. Provider
"full result" messages must never be appended as deltas. If the final snapshot
contains no Unicode letters or numbers, the gateway returns
`session.ended(status="no_speech")` without invoking the LLM or TTS.

On the Function EV device, wake/listen capture continues into a bounded PSRAM
ring while the gateway WebSocket and ASR session are being prepared. The ring
begins with the 400 ms wake-word preroll and is drained before live capture, so
network setup does not create a microphone gap.

Provider adapters implement streaming ASR, LLM and TTS interfaces. The device
protocol deliberately remains unchanged when a provider is replaced.

The Volcengine backend uses the optimized bidirectional ASR endpoint and
aggregates the device's 20 ms frames into the provider-recommended 200 ms PCM
requests. TTS V3 receives raw LLM deltas directly so its server-side sentence
assembler can balance first-audio latency and prosody.

Before selecting `VOICE_BACKEND=volcengine`, grant the speech API key access to
the ASR resource in `VOLC_ASR_RESOURCE_ID`, the TTS resource in
`VOLC_TTS_RESOURCE_ID`, and the configured speaker. The Ark project must also
have the model in `DOUBAO_MODEL` enabled. A general-purpose API key identifies
the caller but does not by itself activate these billable resources.

## Cloud credential handoff

Copy the provider variable names from `.env.example` into the ignored `.env`
file and fill them locally. Do not paste credentials into issues, chat, device
profiles, firmware logs, or commits. The device receives only its independent
`VOICE_DEVICE_TOKEN`.
