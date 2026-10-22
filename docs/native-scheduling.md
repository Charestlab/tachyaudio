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
Stimuli that overlap in time are mixed: their samples are summed and clamped
to [-1, 1].

`wait_rendered()` returns once every sample is in device buffers, possibly
**before onset** for a short stimulus. `wait()` also waits through the predicted
end plus one buffer, so closing afterwards cannot cut the sound. Neither is a
physical measurement. A wait timeout does not cancel playback; use `cancel()` or
`close()`.

| Operation | Behavior |
| --- | --- |
| `schedule(frames, when=...)` | Copies data and returns an id; up to 32 sounds, back to back or overlapping (overlaps mix, summed and clamped) |
| `wait_rendered(stimulus=id)` | Waits until samples reach device buffers, possibly before onset |
| `wait(stimulus=id)` | Also waits through predicted end plus one buffer |
| `cancel(id)` / `cancel()` | Cancels one / all queued sounds; submitted device samples cannot be recalled |
| `close()` | Stops the stream and cancels active records |

Waits default to the latest id. Read results
promptly: inactive records are recycled oldest-first across 32 slots once no
rendered samples remain in flight. Missed onsets drop only that sound;
clock/format/device faults or late callbacks require reopening the stream.

## macOS settings and implementation

The HAL callback adds device and stream latency to output host timestamps to
estimate presentation time, while enforcing the earlier hardware-delivery
deadline. Unequal output-stream latencies are rejected. ALSA's
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
- Listeners fault the stream on sample-rate, device-alive and stream-format
  changes. Latency and buffer-size changes are non-fatal (Core Audio reports them
  as the device settles after a buffer request, so faulting there spuriously
  rejected `start()`); they set a `latency_changed` status flag for re-reading.
- Boundary placement tolerates one Mach host tick (about 42 ns on the tested
  Apple Silicon Mac), avoiding whole-sample gaps. Internal onset residuals may
  therefore be negative by up to one tick.

`status()` reports the clock source and observations, current/maximum output
delay, period/buffer frames, device format/channels, queued count and latest id.
`rt_method="coreaudio-io-thread"` identifies the OS-managed audio thread, not
an independently verified numeric priority.

## Validation

Deterministic tests cover the C renderer and queue; hardware tests cover
cancellation, reuse, overlap mixing and playback while Python holds the GIL.

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

## Immediate playback and tuning

On macOS, `schedule_asap(frames)` copies a sound then starts at the first available
callback. It requires an idle stream, retains the hardware delivery checks, and
adds no Python lead margin. `ScheduledTiming.immediate` identifies this mode;
use `submission_to_onset_seconds`, not `predicted_error_seconds`. Callback phase
makes immediate response variable; use `schedule(when=...)` for a fixed onset.

Run `tachyaudio-audio-report --immediate-only --json immediate.json` to compare
buffers including the device's advertised minimum, rank loaded p99 onset delay,
and independently validate a candidate. `--stimulus-ms` tests different sound
lengths. `--tune-only` also searches deadline leads, refining a failure boundary
in 0.25 ms steps. Failures stay in JSON. These device-clock results are bounded
experiments, not physical timing measurements or safety guarantees.

Apply the reported `latency`; `minimum_lead_time` only affects explicit deadlines.
`--lead-trials`, `--validation-trials` and `--load-workers` control testing.
`start()` warms the stream; first-play startup is additional. Tachypy defaults
to a 1 ms native buffer on macOS; smaller buffers can be selected explicitly.
Low-level `ScheduledOutputStream(latency=None)` preserves the shared buffer.

For a quick, **silent** in-process equivalent at experiment setup, call
`tachyaudio.fastest_safe_settings(device_id=...)`. It sweeps buffers from the
hardware minimum upward, scheduling a short dense burst of zero-filled sounds
under CPU load (nothing audible, no microphone — like the silent scheduling
tests), and returns `FastSettings(latency, minimum_lead_time)` for the fastest
buffer that ran without a dropped/late onset or fault (~1 s):

```python
import tachyaudio as ta
s = ta.fastest_safe_settings(device_id="BuiltInSpeakerDevice")  # under_load=True, workers=4
# ScheduledOutputStream(latency=s.latency, minimum_lead_time=s.minimum_lead_time)
```

It returns that buffer's **default** (margin-carrying) `lead_time`; the
`--tune-only` report instead pushes the lead to its measured minimum. Both are
best-effort device-clock experiments, not guarantees for every future load.

On Linux, use an explicit direct-ALSA output:

```sh
tachyaudio-audio-report --tune-only --output-device hw:0,0 --json linux-audio.json
```

`--immediate-only` works on both platforms. The report labels the implementation:
macOS uses callback-ASAP; Linux currently schedules at default lead + 5 ms,
matching tachypy's existing play-now path. It does not imply that the macOS
optimization has been ported to ALSA. Tests cover idle/load response, failed
batches, fresh validation and explicit-deadline lead refinement. Add
`--stimulus-ms 100 --validation-trials 500 --load-workers 4` for longer validation.
Omit `--tune-only` to include acoustic checks; select `--input-device` as needed.

## PsychPortAudio comparison

The PsychPortAudio comparison is a **standalone tool kept outside this
repository** (`tachyaudio_ptb_compare.py`). It depends only on the public
`tachyaudio` API plus `numpy` and `psychtoolbox`, and requires minimum released
versions of `tachyaudio` and `tachypy`. Run it directly:

```sh
pip install numpy psychtoolbox
python tachyaudio_ptb_compare.py \
    --output-device BuiltInSpeakerDevice --ptb-device 2 --json comparison.json
```

On Linux, use `--output-device hw:CARD,DEVICE` and its matching PTB index
(`--help` lists all options; `--verbose` restores PTB initialization messages).
It reports onsets at two comparable references — **total** (speaker) and
**engine** (DAC) — because native's estimate includes presentation latency while
PTB's excludes the hardware output path; the raw per-backend call-to-onset is not
comparable. Onsets are device-clock estimates, not acoustic measurements, and no
result establishes a physical timing winner. See PTB's
[Start](https://psychtoolbox.org/docs/PsychPortAudio-Start) and
[RunMode](https://psychtoolbox.org/docs/PsychPortAudio-RunMode) contracts. Its
own offline and opt-in hardware tests live in that same file.
