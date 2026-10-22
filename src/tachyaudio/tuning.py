"""Find the fastest glitch-free native-scheduling settings for this machine.

``fastest_safe_settings()`` is **silent**: it schedules only zero-filled buffers
(nothing audible) and reads the scheduler's device-clock state, like the silent
scheduling self-tests. No microphone. It sweeps the device buffer from the
hardware minimum upward and returns the smallest one that played a short dense
burst without a dropped/late onset or stream fault, optionally under CPU load.
Native scheduler only (macOS / Linux with a ``hw:`` device).
"""
from __future__ import annotations

import sys
import time
from array import array
from dataclasses import dataclass

# Buffer sizes (seconds) to try, fastest first. macOS allows sub-millisecond
# buffers; Linux's smallest safe period is larger.
_CANDIDATES_DARWIN = (0.000313, 0.0005, 0.001, 0.002, 0.005, 0.010)
_CANDIDATES_OTHER = (0.001, 0.002, 0.005, 0.010)
_PROBE_FRAMES = 480  # 10 ms silent sounds, scheduled back to back


@dataclass(frozen=True)
class FastSettings:
    """Pass these straight to ``Audio`` / ``ScheduledOutputStream``."""

    latency: float            # device buffer, seconds
    minimum_lead_time: float  # advance notice schedule() needs, seconds


def _cpu_burn():  # module-level so multiprocessing 'spawn' can import it
    v = 1
    while True:
        v = (v * 1664525 + 1013904223) & 0xFFFFFFFF


def _probe(device_id, sample_rate, channels, latency, workers, rounds, per_round):
    """Return (ok, lead_time) after a dense silent burst at one buffer size."""
    import multiprocessing as mp

    from tachyaudio import ScheduledOutputStream

    silence = array("f", [0.0]) * (_PROBE_FRAMES * channels)
    ctx = mp.get_context("spawn")
    procs = [ctx.Process(target=_cpu_burn, daemon=True) for _ in range(workers)]
    for p in procs:
        p.start()
    glitches, lead = 0, 0.0
    try:
        with ScheduledOutputStream(sample_rate=sample_rate, channels=channels,
                                   device_id=device_id, latency=latency) as stream:
            lead = stream.lead_time
            dt = _PROBE_FRAMES / sample_rate
            for _ in range(rounds):
                origin = time.monotonic() + lead + 0.02
                ids = []
                for i in range(per_round):
                    try:
                        ids.append(stream.schedule(silence, when=origin + i * dt))
                    except Exception:
                        glitches += 1
                for stimulus in ids:
                    try:
                        stream.wait(timeout=3, stimulus=stimulus)
                    except Exception:
                        glitches += 1
                if stream.failed:
                    glitches += 1
                    break
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.join(1)
    return glitches == 0, lead


def fastest_safe_settings(device_id=None, *, sample_rate=48_000, channels=1,
                          under_load=True, workers=4):
    """Measure and return the fastest glitch-free :class:`FastSettings` here.

    Silent and quick (about 1-2 s). Opens the output device at successively
    larger buffers and returns the first that scheduled a dense burst with no
    dropped/late onset or fault -- under ``workers`` CPU-load processes when
    ``under_load`` (the realistic case). The returned ``minimum_lead_time`` is
    that buffer's measured advance notice, which already carries a safety margin.

    Raises ``RuntimeError`` if no buffer passed (or the native scheduler is
    unavailable). It changes the shared device buffer briefly on macOS, restored
    on close. This is a best-effort probe, not a guarantee for every future load;
    for the thorough per-machine report use ``tachyaudio-audio-report``.
    """
    candidates = _CANDIDATES_DARWIN if sys.platform == "darwin" else _CANDIDATES_OTHER
    load = workers if under_load else 0
    last_error = None
    for latency in candidates:
        try:
            ok, lead = _probe(device_id, sample_rate, channels, latency, load,
                              rounds=2, per_round=30)
        except Exception as exc:  # unsupported device/buffer: try the next one
            last_error = exc
            continue
        if ok:
            return FastSettings(latency=latency, minimum_lead_time=lead)
    if last_error is not None:
        raise RuntimeError(f"could not probe scheduled output: {last_error}") from last_error
    raise RuntimeError("no scheduled-output buffer played glitch-free; use a larger latency")
