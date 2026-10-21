# Native scheduling (`ScheduledOutputStream`)

Schedule finite float32 stimuli at absolute monotonic times using native device
clocks. tachypy exposes this as `Audio(timing_mode="native")`.

- **macOS:** Core Audio HAL `AudioDeviceIOProc`.
- **Linux:** direct ALSA with an explicit `hw:` device; see
  [Linux setup and validation](linux-scheduling.md).
- **Windows:** unavailable.

## Usage

```python
from array import array
import time
import tachyaudio as ta

# Linux also requires device_id="hw:CARD=...,DEV=...".
with ta.ScheduledOutputStream(sample_rate=48000, channels=1) as stream:
    stimulus = stream.schedule(array("f", [0.0]) * 4800,
                               when=time.monotonic() + 0.150)
    timing = stream.wait(timeout=2, stimulus=stimulus)
```

Construction opens the device; `start()` (or the context manager) starts it
playing silence and waits for initial clock observations. `schedule()` copies the
stimulus and needs `stream.lead_time` of advance notice — on both macOS and Linux
the observed output delay plus one period and 2 ms, or `minimum_lead_time` if
given. It returns an id, and up to 32 stimuli can be queued ahead, back to back,
each waited on or cancelled individually (`wait(stimulus=id)`, `cancel(id)`).
Overlapping stimuli are rejected: there is no mixer.

`wait_rendered()` returns once every sample is in device buffers, possibly
**before onset** for a short stimulus. `wait()` also waits through the predicted
end plus one buffer, so closing afterwards cannot cut the sound. Neither is a
physical measurement. A wait timeout does not cancel playback; use `cancel()` or
`close()`.

| Operation | Behavior |
| --- | --- |
| `schedule(frames, when=...)` | Copies data and returns an id; up to 32 non-overlapping sounds, including back-to-back playback |
| `wait_rendered(stimulus=id)` | Waits until samples reach device buffers, possibly before onset |
| `wait(stimulus=id)` | Also waits through predicted end plus one buffer |
| `cancel(id)` / `cancel()` | Cancels one / all queued sounds; submitted device samples cannot be recalled |
| `close()` | Stops the stream and cancels active records |

Waits default to the latest id. Read results
promptly: inactive records are recycled oldest-first across 32 slots once no
rendered samples remain in flight. Missed onsets drop only that sound;
clock/format/device faults or late callbacks require reopening the stream.

## macOS settings and implementation

The HAL callback uses device-provided output timestamps directly, so ALSA's
status/delay regression and rtkit worker are unnecessary. Rendering uses
lock-free slots, without Python, allocation or mutexes. Copies and allocations
happen on the submitting thread.

- Match the device's current sample rate and float32 format; no resampling.
  Mono is duplicated; otherwise supply the full device channel count.
  Non-finite samples are rejected.
- `latency=None` preserves the current buffer. An explicit value, such as
  `0.005`, requests a supported HAL buffer size in seconds. This is a **shared
  device setting**, restored on close on a best-effort basis only if unchanged
  by another client. Avoid concurrent streams with conflicting settings.
- Listeners detect rate, buffer-size, device-alive and stream-format changes.
- Boundary placement tolerates one Mach host tick (about 42 ns on the tested
  Apple Silicon Mac), avoiding whole-sample gaps. Internal onset residuals may
  therefore be negative by up to one tick.

`status()` reports the clock source and observations, current/maximum output
delay, period/buffer frames, device format/channels, queued count and latest id.
`rt_method="coreaudio-io-thread"` identifies the OS-managed audio thread, not
an independently verified numeric priority.

## Validation

Deterministic tests cover the C renderer and queue; hardware tests cover
cancellation, reuse, overlap rejection and playback while Python holds the GIL.

```sh
python setup.py build_ext --inplace
TACHYAUDIO_TEST_HARDWARE=1 PYTHONPATH=src python -m unittest discover -s tests
PYTHONPATH=src python examples/measure_scheduled_timing.py \
    --trials 200 --output build/native-timing.json
PYTHONPATH=src python examples/measure_coreaudio_loopback.py \
    --latency .005 --load --trials 24 --output build/acoustic-timing.json
```

Native timing reports are device-clock estimates, not physical measurements.
The acoustic probe measures intervals through the integrated speakers/microphone,
removing constant path delay and linear capture-clock drift. Neither establishes
absolute onset accuracy or superiority to PsychPortAudio.

For that comparison, use matched hardware/settings and an external acquisition
clock with a calibrated reference trigger. Report bias, jitter, error percentiles,
worst case and all failures under representative load; keep calibration trials
separate. See [electrical loopback](electrical-loopback.md), Apple's
[IOProc reference](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc),
and Psychtoolbox's [external timing test](https://psychtoolbox.org/docs/PsychPortAudioDataPixxTimingTest).
