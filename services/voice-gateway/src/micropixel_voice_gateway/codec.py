from __future__ import annotations

from .protocol import CHANNELS, FRAME_DURATION_MS, SAMPLE_RATE


class CodecUnavailable(RuntimeError):
    pass


class OpusCodec:
    """Lazy raw-Opus codec so protocol tests do not require libopus."""

    def __init__(self, bitrate: int = 24_000) -> None:
        try:
            import opuslib  # type: ignore[import-not-found]
        except ImportError as error:
            raise CodecUnavailable("install the 'opus' extra and system libopus") from error
        self._opuslib = opuslib
        self._encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
        self._encoder.bitrate = bitrate
        self._decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)
        self.frame_samples = SAMPLE_RATE * FRAME_DURATION_MS // 1_000

    def decode(self, packet: bytes) -> bytes:
        return self._decoder.decode(packet, self.frame_samples, False)

    def encode(self, pcm16: bytes) -> bytes:
        if len(pcm16) != self.frame_samples * 2:
            raise ValueError("PCM chunk must contain exactly one 20 ms frame")
        return self._encoder.encode(pcm16, self.frame_samples)
