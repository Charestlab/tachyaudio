"""Experimental finite-stimulus scheduling against the macOS device clock."""

from __future__ import annotations

import math
import time
from concurrent.futures import CancelledError
from dataclasses import dataclass
from typing import Any

from tachyaudio._errors import BackendUnavailable, StreamClosed, TachyAudioError
from tachyaudio._stream import StreamConfig, _validate_frames, _validate_timeout


class SchedulingError(TachyAudioError):
    """The native renderer could not meet a stimulus timing requirement."""


@dataclass(frozen=True, slots=True)
class ScheduledTiming:
    """Device-clock estimates, not independently measured physical onset."""

    requested_ns: int
    predicted_onset_ns: int
    render_callback_ns: int
    render_complete_ns: int
    predicted_end_ns: int
    rendered_frames: int

    @property
    def predicted_error_seconds(self) -> float:
        return (self.predicted_onset_ns - self.requested_ns) / 1e9


class ScheduledOutputStream:
    """Opt-in macOS scheduler for one finite stimulus at a time.

    Opens the selected device without changing its rate or buffer configuration.
    Data must match the device's native rate. Mono is duplicated to all output
    channels; otherwise use the device's complete channel count. Timing uses
    Core Audio's output timestamps and still requires external validation.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48_000,
        channels: int = 2,
        device_id: str | None = None,
        minimum_lead_time: float = 0.050,
    ) -> None:
        self.config = StreamConfig(sample_rate=sample_rate, channels=channels, device_id=device_id)
        if not math.isfinite(minimum_lead_time) or minimum_lead_time < 0:
            raise ValueError("minimum_lead_time must be finite and nonnegative")
        if minimum_lead_time * 1e9 >= 2**64:
            raise ValueError("minimum_lead_time exceeds the native clock range")
        self.minimum_lead_time = minimum_lead_time
        try:
            from tachyaudio import _native
        except ImportError as error:
            raise BackendUnavailable("native scheduling requires the macOS extension") from error
        factory = getattr(_native, "ScheduledOutputStream", None)
        if factory is None:
            raise BackendUnavailable("native scheduling is currently implemented only for macOS")
        self._handle = factory(sample_rate, channels, device_id)
        self._closed = False

    def __enter__(self) -> ScheduledOutputStream:
        try:
            self.start()
        except BaseException:
            self.close()
            raise
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def start(self) -> None:
        """Start device callbacks, initially emitting silence."""
        self._require_open()
        self._handle.start()

    def schedule(self, frames: Any, *, when: float) -> None:
        """Copy a whole float32 stimulus ahead of an absolute monotonic deadline.

        Rejects overlapping requests and deadlines inside minimum_lead_time.
        Python does not need to run again at onset. Call wait() to observe the
        render result; a late native callback drops the stimulus and reports an
        error rather than silently shifting onset.
        """
        self._require_open()
        if not math.isfinite(when) or when <= 0:
            raise ValueError("when must be a finite positive monotonic time")
        target_ns = round(when * 1e9)
        if not 0 < target_ns < 2**64:
            raise ValueError("when exceeds the native clock range")
        if target_ns - time.monotonic_ns() < self.minimum_lead_time * 1e9:
            raise SchedulingError("insufficient advance notice for scheduled onset")
        view, _, _ = _validate_frames(frames, self.config.channels)
        self._handle.schedule(view, target_ns, round(self.minimum_lead_time * 1e9))

    def status(self) -> dict[str, Any]:
        """Return experimental native status and diagnostic counters."""
        return dict(self._handle.status())

    def wait(self, timeout: float | None = None) -> ScheduledTiming:
        """Wait for rendering and estimated device-clock completion.

        Includes one device-buffer interval after the predicted final sample
        before returning, so an immediate close does not stop an onset that is
        still in the future. This remains an estimate, not external validation.
        """
        _validate_timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        result = self.wait_rendered(timeout)
        tail_ns = round(self.status().get("buffer_frames", 0) / self.config.sample_rate * 1e9)
        finish_ns = result.predicted_end_ns + tail_ns
        while time.monotonic_ns() < finish_ns:
            if self._closed:
                raise CancelledError("stream closed before estimated playback completion")
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("estimated playback completion exceeded timeout")
            time.sleep(0.001)
        return result

    def wait_rendered(self, timeout: float | None = None) -> ScheduledTiming:
        """Wait until the stimulus is rendered into device buffers.

        This is not a physical-playback completion guarantee. A timeout leaves
        the stimulus scheduled; call cancel() or close() explicitly to stop it.
        """
        _validate_timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        failures = {
            6: "native renderer missed the requested onset; stimulus dropped",
            7: "device output timestamp is unavailable",
            8: "device sample clock changed during playback",
            9: "render callback arrived after its output deadline",
            10: "device output format changed or is unsupported",
        }
        while True:
            status = self.status()
            state = status["state"]
            if state == 4:
                return ScheduledTiming(**{name: status[name] for name in (
                    "requested_ns", "predicted_onset_ns", "render_callback_ns",
                    "render_complete_ns",
                    "predicted_end_ns", "rendered_frames",
                )})
            if state == 5:
                raise CancelledError("scheduled stimulus cancelled")
            if state in failures:
                raise SchedulingError(failures[state])
            if state == 0:
                raise SchedulingError("no stimulus has been scheduled")
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("scheduled stimulus did not finish rendering before timeout")
            time.sleep(0.001)

    def cancel(self) -> None:
        """Stop supplying this stimulus at the next native render callback.

        Samples already handed to the device cannot be recalled.
        """
        self._require_open()
        self._handle.cancel()

    def close(self) -> None:
        if not self._closed:
            self._handle.close()
            self._closed = True

    def _require_open(self) -> None:
        if self._closed:
            raise StreamClosed("scheduled output stream is closed")
