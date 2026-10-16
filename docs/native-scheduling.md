# Native scheduling: experimental macOS implementation

An initial Linux ALSA implementation now uses the same public API. See
[Ubuntu handoff](ubuntu-scheduling.md) for its different timing basis and pending
hardware validation. Windows scheduled output remains unimplemented.

The objective is to match and, if independent measurements support it, surpass
PsychPortAudio's timing performance. No superiority or physical-onset accuracy
claim has been established. The existing `OutputStream` and default tachypy
scheduler do not automatically use this path. The companion tachypy integration
adds explicit `Audio(timing_mode="native")`; it requires this development build.

See [electrical loopback](electrical-loopback.md) for practical measurement wiring,
clock-reference requirements, and the distinction from dedicated TDT hardware.

## Architecture

`ScheduledOutputStream` uses a Core Audio `AudioDeviceIOProc` directly, rather
than the existing AudioQueue ring-buffer path. Python supplies the complete
stimulus and an absolute monotonic deadline ahead of time. The C callback uses
the device-provided output timestamp to select the first sample position within
an output buffer. It supplies silence until that position, then the stimulus.

The renderer does not call Python, allocate memory, acquire a mutex, or wait for
the GIL. A single stimulus slot is published with lock-free C atomics; the data
copy and allocation happen on the submitting thread. Float32 data at the
device's current sample rate is required; the scheduler does not silently
resample or change shared device configuration. Mono is duplicated to every
device output channel. Otherwise the full device channel count is required.

Missed onset windows, missing device timestamps, sample-clock discontinuities,
format mismatches, and late render callbacks are reported as errors. A late
onset is dropped instead of silently played late. If a failure occurs after a
stimulus has begun, already delivered samples cannot be recalled. Cancellation
likewise stops subsequent rendering, not samples already handed to the device.

## Usage

```python
from array import array
import time
import tachyaudio as ta

with ta.ScheduledOutputStream(sample_rate=48000, channels=1) as stream:
    samples = array("f", [0.0]) * 4800
    stream.schedule(samples, when=time.monotonic() + 0.150)
    timing = stream.wait(timeout=2)
    print(timing.predicted_error_seconds)
```

Construction opens the device; `start()` (or the context manager) starts the
silent engine. `schedule()` copies a finite stimulus and publishes its deadline.
It requires at least 50 ms of advance notice by default, including after copying
the data. This is a conservative submission policy, not an onset delay or a
proven minimum requirement. Requests cannot overlap on one scheduled stream.

`wait_rendered()` returns once all samples have been copied to device buffers.
It may return **before onset** for a short stimulus. Keep the device running.
`wait()` additionally waits through the predicted end plus one device-buffer
interval to avoid immediately closing while the scheduled audio is still ahead
of the device clock. Neither is an independent physical-completion measurement.
A wait timeout does not cancel playback; use `cancel()` or `close()` explicitly.

The current API is experimental, macOS-only, and does not provide a playlist or
overlap mixer. Linux/Windows precision scheduling and tachypy integration need
their own implementation and validation. Normal streaming remains available.

## Validation implemented

- The actual C renderer is tested without hardware for within-buffer placement,
  continuation across buffers, silent lead-in, missed onsets, discontinuities,
  and rounding at buffer boundaries.
- Hardware tests exercise cancellation, reuse, overlapping-request rejection,
  and a Python busy loop spanning the requested onset.
- `examples/measure_scheduled_timing.py` records idle, competing-thread, and
  Python-busy-across-onset trials, including every rejected or failed request.

```sh
python setup.py build_ext --inplace
TACHYAUDIO_TEST_HARDWARE=1 PYTHONPATH=src python -m unittest discover -s tests
PYTHONPATH=src python examples/measure_scheduled_timing.py \
    --trials 200 --output build/native-timing.json
```

The sample-placement residual is calculated from the same device timestamp used
to choose the sample. A residual below one sample is an internal correctness
check, **not proof of sub-sample physical timing accuracy**. Render lead measures
how early native code finished writing that sample, so positive margins support
the claim that Python wake-up at onset was unnecessary.

## Standard for a PsychPortAudio comparison

Predeclare targets and compare both implementations on the same output device,
sample rate, operating system, trial schedule, and load conditions. Record
software versions and configuration. Run separate tests for:

1. Scheduled absolute onset error and trial-to-trial jitter.
2. Minimum reliable advance notice, sweeping 5, 10, 25, 50, and 100 ms.
3. Unscheduled immediate-start latency (a different capability).
4. Missed deadlines, underruns, cancellations, and recovery after device changes.

Record an electrical audio output and a reference trigger on a common external
acquisition clock. Use a calibrated synchronization method relating requested
host times to that clock; a trigger emitted from an ordinary Python/MATLAB
thread is not a precise reference. An acoustic microphone test is a separate
measurement of the complete speaker path. Do not use software-reported onset
as the ground truth for either library.

Report signed bias, standard deviation, absolute-error p95/p99/p99.9, worst case,
and failure counts. Preserve failures and outliers. Report uncorrected errors
and, separately, errors after a fixed calibration obtained from different
trials. Do not recalibrate each stimulus or subtract the observed trial error.

Initial engineering targets (not achieved claims): calibrated onset jitter
below 0.1 ms standard deviation, p99.9 absolute error below 0.5 ms, and no missed
deadlines in at least 10,000 trials per stated condition. Require repeated
sessions, representative experiment load, and confidence intervals. These are
baseline gates; "better than PsychPortAudio" additionally requires lower error
and/or reliable lead-time requirements without worse failure rates in the paired
comparison. Zero failures in a finite run is not a guarantee of zero risk.

References: Apple's [AudioDeviceIOProc documentation](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc),
Psychtoolbox's [scheduled playback API](https://psychtoolbox.org/docs/PsychPortAudio-Start),
and its [external timing test](https://psychtoolbox.org/docs/PsychPortAudioDataPixxTimingTest).
