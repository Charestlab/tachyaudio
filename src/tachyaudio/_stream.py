"""Stream API shared by playback and capture backends."""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

from tachyaudio._backend import get_backend
from tachyaudio._errors import StreamClosed


_POLL_INTERVAL = 0.001


def _validate_timeout(timeout: float | None) -> None:
    if timeout is not None and timeout < 0:
        raise ValueError("timeout cannot be negative")


def _deadline(timeout: float | None) -> float | None:
    return None if timeout is None else time.monotonic() + timeout


def _timed_out(deadline: float | None) -> bool:
    return deadline is not None and time.monotonic() >= deadline


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
    hardware_latency: float | None = None
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
        if self.hardware_latency is not None and self.hardware_latency < 0:
            raise ValueError("hardware_latency cannot be negative")
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
        _validate_timeout(timeout)
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

    def write_all(self, frames: Any, timeout: float | None = None) -> int:
        """Write all interleaved frames, waiting for ring-buffer capacity.

        Returns the number of frames accepted. Raises `TimeoutError` if the
        stream does not accept the complete buffer before `timeout`.
        """

        self._require_open()
        _validate_timeout(timeout)
        view = memoryview(frames).cast("B")
        frame_bytes = self.config.channels * 4
        if len(view) == 0 or len(view) % frame_bytes != 0:
            raise ValueError("frames must contain whole interleaved float32 frames")

        deadline = _deadline(timeout)
        total_frames = len(view) // frame_bytes
        written_frames = 0
        while written_frames < total_frames:
            start = written_frames * frame_bytes
            remaining_frames = total_frames - written_frames
            accepted = self.write(view[start:])
            if accepted < 0:
                raise RuntimeError("backend returned a negative frame count")
            if accepted > remaining_frames:
                raise RuntimeError("backend accepted more frames than requested")
            written_frames += accepted
            if written_frames >= total_frames:
                return written_frames
            if _timed_out(deadline):
                raise TimeoutError("audio write did not complete before timeout")
            time.sleep(_POLL_INTERVAL)

        return written_frames

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
        stream.start()
        stream.write_all(frames, timeout)
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

    def read_exactly(self, frame_count: int, timeout: float | None = None) -> memoryview:
        """Read exactly `frame_count` frames, waiting for captured input.

        Raises `TimeoutError` if the requested frame count is not available
        before `timeout`.
        """

        self._require_open()
        if frame_count < 1:
            raise ValueError("frame_count must be positive")
        _validate_timeout(timeout)

        frame_bytes = self.config.channels * 4
        deadline = _deadline(timeout)
        chunks: list[memoryview] = []
        captured_frames = 0
        while captured_frames < frame_count:
            chunk = self.read(frame_count - captured_frames)
            if len(chunk) % frame_bytes != 0:
                raise RuntimeError("backend returned partial frame bytes")
            chunk_frames = len(chunk) // frame_bytes
            if chunk_frames > frame_count - captured_frames:
                raise RuntimeError("backend returned more frames than requested")
            if chunk_frames:
                chunks.append(chunk)
                captured_frames += chunk_frames
                continue
            if _timed_out(deadline):
                raise TimeoutError("audio read did not complete before timeout")
            time.sleep(_POLL_INTERVAL)

        output = bytearray(frame_count * frame_bytes)
        offset = 0
        for chunk in chunks:
            output[offset : offset + len(chunk)] = chunk
            offset += len(chunk)
        return memoryview(output)

    def stats(self) -> StreamStats:
        """Return current stream counters."""

        self._require_open()
        return self._handle.stats()

    def _require_open(self) -> None:
        if self._closed:
            raise StreamClosed("stream is closed")
