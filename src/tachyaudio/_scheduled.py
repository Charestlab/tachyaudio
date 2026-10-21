"""Experimental finite-stimulus scheduling using native playback timestamps."""

from __future__ import annotations

import math
import sys
import time
from concurrent.futures import CancelledError
from dataclasses import dataclass
from typing import Any

from tachyaudio._errors import BackendUnavailable, StreamClosed, TachyAudioError
from tachyaudio._stream import StreamConfig, _validate_frames, _validate_timeout

_STREAM_FAULTS = frozenset({7, 8, 9, 10})


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
    """Opt-in native scheduler for one finite stimulus at a time.

    macOS uses Core Audio output timestamps on the device's current
    configuration. Linux opens an explicit ALSA ``hw:`` device directly
    (bypassing PulseAudio/PipeWire) and places onsets with a device-clock model
    fitted to ALSA status reports. Neither is an externally verified onset.

    Parameters
    ----------
    sample_rate:
        Frames per second of the stimuli. Must be a rate the device supports;
        nothing is resampled.
    channels:
        Channels per frame of the stimuli. Mono is duplicated to every device
        channel; otherwise give the device's full channel count.
    device_id:
        macOS: device id or ``None`` for the default output. Linux: required
        ``hw:CARD,DEVICE`` (``aplay -l``), not in use by another program.
    minimum_lead_time:
        Seconds of advance notice ``schedule()`` requires. ``None`` (default):
        on Linux, derived from the device after ``start()`` (see
        ``lead_time``); on macOS, 50 ms.
    latency:
        Linux only. Requested ALSA buffer in seconds, as for ``OutputStream``.
        ``None`` (default) chooses 5 ms when the worker can obtain real-time
        priority (rtkit or an rtprio grant), otherwise 20 ms. Smaller shortens
        the device output delay and thus ``lead_time``; it does not change
        onset precision.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48_000,
        channels: int = 2,
        device_id: str | None = None,
        minimum_lead_time: float | None = None,
        latency: float | None = None,
    ) -> None:
        self.config = StreamConfig(sample_rate=sample_rate, channels=channels, device_id=device_id)
        if minimum_lead_time is not None:
            if not math.isfinite(minimum_lead_time) or minimum_lead_time < 0:
                raise ValueError("minimum_lead_time must be finite and nonnegative")
            if minimum_lead_time * 1e9 >= 2**64:
                raise ValueError("minimum_lead_time exceeds the native clock range")
        self.minimum_lead_time = minimum_lead_time
        if sys.platform.startswith("linux") and not (device_id or "").startswith("hw:"):
            from tachyaudio._alsa_devices import describe_alsa_outputs

            raise ValueError(
                "Linux scheduled output needs device_id='hw:...' (direct ALSA, not "
                f"PulseAudio/PipeWire); got {device_id!r}.\n\n{describe_alsa_outputs()}"
            )
        if latency is not None:
            if not sys.platform.startswith("linux"):
                raise ValueError("latency is supported only by the Linux ALSA scheduler")
            if not math.isfinite(latency) or not 0.001 <= latency <= 1.0:
                raise ValueError("latency must be between 0.001 and 1.0 seconds")
        try:
            if sys.platform.startswith("linux"):
                from tachyaudio import _scheduled_alsa as _native
            else:
                from tachyaudio import _native
        except ImportError as error:
            raise BackendUnavailable(
                "native scheduling requires a supported compiled extension"
            ) from error
        factory = getattr(_native, "ScheduledOutputStream", None)
        if factory is None:
            raise BackendUnavailable("native scheduling is available only on macOS and Linux")
        arguments: tuple[Any, ...] = (sample_rate, channels, device_id)
        if latency is not None:
            arguments += (round(latency * 1e6),)
        try:
            self._handle = factory(*arguments)
        except RuntimeError as error:
            if not sys.platform.startswith("linux"):
                raise
            from tachyaudio._alsa_devices import describe_alsa_outputs

            raise RuntimeError(
                f"Could not open {device_id!r} at {sample_rate} Hz with {channels} channel(s): "
                "the device is in use, absent, or does not support that rate or channel "
                f"count.\n\n{describe_alsa_outputs()}"
            ) from error
        self._closed = False
        self._latest: int | None = None

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

    def schedule(self, frames: Any, *, when: float) -> int:
        """Copy a whole float32 stimulus ahead of an absolute monotonic deadline.

        Returns the stimulus id for ``wait``/``cancel``. On Linux up to 32
        stimuli can be queued ahead, back to back if desired; on macOS one at a
        time. A stimulus overlapping one already queued, or due sooner than
        ``lead_time``, is rejected. Python does not need to run again at onset.
        A stimulus whose onset can no longer be met is dropped and reported by
        ``wait`` rather than played late.
        """
        self._require_open()
        if not math.isfinite(when) or when <= 0:
            raise ValueError("when must be a finite positive monotonic time")
        target_ns = round(when * 1e9)
        if not 0 < target_ns < 2**64:
            raise ValueError("when exceeds the native clock range")
        lead_ns = round(self.lead_time * 1e9)
        if target_ns - time.monotonic_ns() < lead_ns:
            raise SchedulingError("insufficient advance notice for scheduled onset")
        view, _, _ = _validate_frames(frames, self.config.channels)
        stimulus = self._handle.schedule(view, target_ns, lead_ns)
        # The macOS renderer holds a single stimulus and does not number them.
        self._latest = stimulus if isinstance(stimulus, int) else (self._latest or 0) + 1
        return self._latest

    @property
    def lead_time(self) -> float:
        """Advance notice, in seconds, that schedule() requires.

        Equals ``minimum_lead_time`` when one was given. Otherwise Linux derives
        it from the device after start(): the largest output delay observed,
        plus one period and 2 ms. macOS uses 50 ms.
        """
        if self.minimum_lead_time is not None:
            return self.minimum_lead_time
        if not sys.platform.startswith("linux"):
            return 0.050
        status = self._handle.status()
        period = status["period_frames"] / self.config.sample_rate
        return float(status["output_delay_max_us"] / 1e6 + period + 0.002)

    def status(self) -> dict[str, Any]:
        """Return experimental native status and diagnostic counters."""
        return dict(self._handle.status())

    @property
    def failed(self) -> bool:
        """True once the stream is closed or has a stream-level fault; reopen it."""
        return self._closed or self._handle.status()["state"] in _STREAM_FAULTS

    def wait(self, timeout: float | None = None, stimulus: int | None = None) -> ScheduledTiming:
        """Wait for rendering and estimated device-clock completion.

        Waits for ``stimulus`` (an id from ``schedule``), by default the most
        recently scheduled one.

        Includes one device-buffer interval after the predicted final sample
        before returning, so an immediate close does not stop an onset that is
        still in the future. This remains an estimate, not external validation.
        """
        _validate_timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        result = self.wait_rendered(timeout, stimulus)
        tail_ns = round(self.status().get("buffer_frames", 0) / self.config.sample_rate * 1e9)
        finish_ns = result.predicted_end_ns + tail_ns
        while time.monotonic_ns() < finish_ns:
            if self._closed:
                raise CancelledError("stream closed before estimated playback completion")
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("estimated playback completion exceeded timeout")
            time.sleep(0.001)
        return result

    def wait_rendered(self, timeout: float | None = None,
                      stimulus: int | None = None) -> ScheduledTiming:
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
            status = self._record(stimulus)
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

    def cancel(self, stimulus: int | None = None) -> None:
        """Stop supplying ``stimulus``, or every queued stimulus if None.

        Samples already handed to the device cannot be recalled.
        """
        self._require_open()
        if sys.platform.startswith("linux"):
            self._handle.cancel(stimulus or 0)
        elif stimulus is None or stimulus == self._latest:
            self._handle.cancel()

    def _record(self, stimulus: int | None) -> dict[str, Any]:
        if sys.platform.startswith("linux"):
            if self._latest is None:
                raise SchedulingError("no stimulus has been scheduled")
            try:
                return dict(self._handle.result(stimulus or 0))
            except LookupError as error:
                raise ValueError(str(error)) from error
        if stimulus is not None and stimulus != self._latest:
            raise ValueError("this platform keeps only the latest stimulus")
        return self.status()

    def close(self) -> None:
        if not self._closed:
            self._handle.close()
            self._closed = True

    def _require_open(self) -> None:
        if self._closed:
            raise StreamClosed("scheduled output stream is closed")
