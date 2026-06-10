"""Stream API shared by playback and capture backends."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from tachyaudio._backend import get_backend
from tachyaudio._errors import StreamClosed


@dataclass(frozen=True, slots=True)
class StreamConfig:
    """Audio stream configuration."""

    sample_rate: int = 48_000
    channels: int = 2
    block_size: int | None = None
    device_id: str | None = None
    latency: float | None = None
    dtype: str = "float32"

    def __post_init__(self) -> None:
        if self.sample_rate < 1:
            raise ValueError("sample_rate must be positive")
        if self.channels < 1:
            raise ValueError("channels must be positive")
        if self.block_size is not None and self.block_size < 1:
            raise ValueError("block_size must be positive")
        if self.latency is not None and self.latency <= 0:
            raise ValueError("latency must be positive")
        if self.dtype != "float32":
            raise ValueError("only float32 streams are currently part of the public API")


@dataclass(frozen=True, slots=True)
class StreamStats:
    """Runtime counters reported by a stream backend."""

    frames_processed: int = 0
    underruns: int = 0
    overruns: int = 0
    estimated_latency: float | None = None
    queued_frames: int = 0
    queued_latency: float = 0.0
    buffer_size: int | None = None

    def __post_init__(self) -> None:
        if self.frames_processed < 0:
            raise ValueError("frames_processed cannot be negative")
        if self.underruns < 0:
            raise ValueError("underruns cannot be negative")
        if self.overruns < 0:
            raise ValueError("overruns cannot be negative")
        if self.estimated_latency is not None and self.estimated_latency < 0:
            raise ValueError("estimated_latency cannot be negative")
        if self.queued_frames < 0:
            raise ValueError("queued_frames cannot be negative")
        if self.queued_latency < 0:
            raise ValueError("queued_latency cannot be negative")
        if self.buffer_size is not None and self.buffer_size < 1:
            raise ValueError("buffer_size must be positive")


class OutputStream:
    """Playback stream.

    This class is intentionally thin. Backend implementations own real-time
    audio resources and expose a small handle with `start`, `stop`, `write`,
    and `stats` methods.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48_000,
        channels: int = 2,
        block_size: int | None = None,
        device_id: str | None = None,
        latency: float | None = None,
        dtype: str = "float32",
    ) -> None:
        self.config = StreamConfig(
            sample_rate=sample_rate,
            channels=channels,
            block_size=block_size,
            device_id=device_id,
            latency=latency,
            dtype=dtype,
        )
        self._handle = get_backend().open_output_stream(self.config)
        self._closed = False

    def __enter__(self) -> OutputStream:
        self.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @property
    def closed(self) -> bool:
        """Whether the stream has been closed."""

        return self._closed

    def start(self) -> None:
        """Start playback."""

        self._require_open()
        self._handle.start()

    def stop(self) -> None:
        """Stop playback without releasing resources."""

        self._require_open()
        self._handle.stop()

    def drain(self, timeout: float | None = None) -> bool:
        """Wait until queued frames have been consumed.

        Returns `True` when the stream drains before `timeout`, or `False` on
        timeout. `timeout=None` waits indefinitely.
        """

        self._require_open()
        if timeout is not None and timeout < 0:
            raise ValueError("timeout cannot be negative")
        return self._handle.drain(timeout)

    def flush(self) -> None:
        """Discard queued frames without closing the stream."""

        self._require_open()
        self._handle.flush()

    def close(self) -> None:
        """Release stream resources."""

        if not self._closed:
            self._handle.close()
            self._closed = True

    def write(self, frames: Any) -> int:
        """Write interleaved frames to the playback stream."""

        self._require_open()
        return self._handle.write(frames)

    def stats(self) -> StreamStats:
        """Return current stream counters."""

        self._require_open()
        return self._handle.stats()

    def _require_open(self) -> None:
        if self._closed:
            raise StreamClosed("stream is closed")


def play(
    frames: Any,
    *,
    sample_rate: int = 48_000,
    channels: int = 2,
    block_size: int | None = None,
    device_id: str | None = None,
    latency: float | None = None,
    dtype: str = "float32",
    timeout: float | None = None,
) -> StreamStats:
    """Play interleaved frames and return final stream statistics."""

    stream = OutputStream(
        sample_rate=sample_rate,
        channels=channels,
        block_size=block_size,
        device_id=device_id,
        latency=latency,
        dtype=dtype,
    )
    try:
        stream.write(frames)
        stream.start()
        if not stream.drain(timeout):
            raise TimeoutError("audio playback did not drain before timeout")
        return stream.stats()
    finally:
        stream.close()


class InputStream:
    """Capture stream."""

    def __init__(
        self,
        *,
        sample_rate: int = 48_000,
        channels: int = 1,
        block_size: int | None = None,
        device_id: str | None = None,
        latency: float | None = None,
        dtype: str = "float32",
    ) -> None:
        self.config = StreamConfig(
            sample_rate=sample_rate,
            channels=channels,
            block_size=block_size,
            device_id=device_id,
            latency=latency,
            dtype=dtype,
        )
        self._handle = get_backend().open_input_stream(self.config)
        self._closed = False

    def __enter__(self) -> InputStream:
        self.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @property
    def closed(self) -> bool:
        """Whether the stream has been closed."""

        return self._closed

    def start(self) -> None:
        """Start capture."""

        self._require_open()
        self._handle.start()

    def stop(self) -> None:
        """Stop capture without releasing resources."""

        self._require_open()
        self._handle.stop()

    def flush(self) -> None:
        """Discard queued captured frames without closing the stream."""

        self._require_open()
        self._handle.flush()

    def close(self) -> None:
        """Release stream resources."""

        if not self._closed:
            self._handle.close()
            self._closed = True

    def read(self, frame_count: int) -> memoryview:
        """Read interleaved captured frames."""

        self._require_open()
        if frame_count < 1:
            raise ValueError("frame_count must be positive")
        return self._handle.read(frame_count)

    def stats(self) -> StreamStats:
        """Return current stream counters."""

        self._require_open()
        return self._handle.stats()

    def _require_open(self) -> None:
        if self._closed:
            raise StreamClosed("stream is closed")
