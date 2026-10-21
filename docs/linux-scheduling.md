# Linux scheduled output (direct ALSA)

`ScheduledOutputStream` on Linux, also used by tachypy's
`Audio(timing_mode="native")`. It plays through ALSA directly, bypassing
PulseAudio/PipeWire, and a native real-time thread places each stimulus on the
device clock, so Python does not need to run at onset. Ordinary tachyaudio
streams and tachypy's default `timing_mode="software"` do not use it (see the
README, "Linux: which audio path is used").

Status: experimental, validated on Ubuntu 24.04 (see [Validation](#validation)).
Until the next release, install tachyaudio from source (`pip install -e .`);
0.2.0b2 does not include it.

## Quick start

**1. Choose the output device.** Open a stream without `device_id`; the error
lists every playback device:

```sh
python3 -c "import tachyaudio; tachyaudio.ScheduledOutputStream()"
```

```text
  device_id                 short   type     name                                    status
  hw:CARD=NVidia,DEV=3      hw:0,3  screen   LG ULTRAGEAR+ (HDA NVidia)              free
  hw:CARD=NVidia,DEV=7      hw:0,7  screen   HDMI 1 (HDA NVidia)                     no screen connected
  hw:CARD=PCH,DEV=0         hw:1,0  jacks    ALC897 Analog (HDA Intel PCH)           free
  hw:CARD=Wireless,DEV=0    hw:2,0  USB      HyperX Cloud Alpha Wireless             in use by pipewire (pid 4405)
```

`screen` is the speakers of a monitor or TV over HDMI/DisplayPort, `jacks` the
computer's headphone/speaker sockets, `digital` S/PDIF, `USB` a USB headset or
interface. Prefer the stable `hw:CARD=...` form; the numbers in `hw:N,M` can
change when USB devices are plugged in. `tachyaudio.list_alsa_outputs()`
returns the same list.

**2. Play.** Only the device, the sample rate of your stimuli and the mode are
needed; the rest is chosen automatically.

```python
import time
import numpy as np
from tachypy.audio import Audio

tone = (0.1 * np.sin(2 * np.pi * 1000 * np.arange(4800) / 48000)).astype(np.float32)

audio = Audio(sample_rate=48000, device_id="hw:CARD=PCH,DEV=0", timing_mode="native")
try:
    audio.prepare()
    timing = audio.play(tone, when=time.monotonic() + 0.100).result()
finally:
    audio.close()
```

The same with tachyaudio alone:

```python
import tachyaudio as ta

with ta.ScheduledOutputStream(sample_rate=48000, channels=1,
                              device_id="hw:CARD=PCH,DEV=0") as stream:
    stream.schedule(tone, when=time.monotonic() + 0.100)
    print(stream.wait(timeout=2))
```

`play`/`schedule` need `stream.lead_time` of advance notice (8 ms on the
onboard ALC897, 12 ms on the USB headset tested). Keep one prepared stream for
the whole experiment.

**3. Queue several sounds** (up to 32 per stream), even back to back; each
`play` returns its own future, each `schedule` its own id for
`wait(stimulus=...)` and `cancel(...)`:

```python
start = time.monotonic() + 0.100
audio.play(tone, when=start)
audio.play(tone, when=start + len(tone) / 48000)   # starts exactly where the first ends
```

## Parameters

Same names in tachyaudio's `ScheduledOutputStream` and tachypy's `Audio`.
Where defaults differ they are given as tachyaudio / tachypy.

| Parameter | Default | Meaning | Set it when |
|---|---|---|---|
| `device_id` | required | `hw:CARD=Name,DEV=M` or `hw:N,M`, from the list above. | Always. |
| `sample_rate` | 48000 / 44100 | Rate of your stimuli. Must be supported by the device (`aplay -D hw:N,M --dump-hw-params /dev/zero`); nothing is resampled. | Always. |
| `channels` | 2 / 1 | Channels of your stimuli. Mono is copied to every device channel. | Multichannel stimuli. |
| `latency` | automatic | ALSA buffer in seconds, as for `OutputStream(latency=...)`: 5 ms when real-time priority is available, else 20 ms. Changes the required advance notice, not the precision. | Rarely: more margin on an unusual or overloaded machine. |
| `minimum_lead_time` | automatic | Advance notice `schedule`/`play` require: largest measured output delay + one period + 2 ms. Read it as `stream.lead_time`. | To impose one fixed value on every device. |
| `timeout` (tachypy) | `None` | Limit on waiting for completion. | Optional. |

`status()` shows what was chosen and how the stream is doing: `buffer_frames`,
`buffer_automatic`, `period_frames`, `device_channels`, `device_format`,
`rt_method`, `rt_priority`, `output_delay_us`, `output_delay_max_us`, and the
clock-model diagnostics `clock_observations`, `clock_ppm`, `clock_jitter_us`,
`clock_resets`.

## Troubleshooting

| Message or symptom | What to do |
|---|---|
| `needs device_id='hw:...'` | Pass a `device_id` from the printed list. |
| `Could not open ...`, device shown `in use by ...` | Close the program holding it. PipeWire keeps a device open while any application uses it (the GNOME Sound settings panel, for example, keeps the microphone open). |
| `Could not open ...`, device free | The device does not support that `sample_rate` or channel count; check with `aplay -D hw:N,M --dump-hw-params /dev/zero`. |
| `insufficient advance notice` | Schedule further ahead than `stream.lead_time`. |
| `overlaps a stimulus already scheduled` | Two sounds cannot overlap on one stream; shift `when` (exactly back to back is allowed) or mix them into one array. |
| `too many stimuli queued (32)` | Wait for earlier sounds to finish before queuing more. |
| `status()['rt_method'] == 'none'` | No real-time priority (no rtkit, no rtprio grant): the automatic buffer is 20 ms and heavy CPU load can still cause faults. Install/enable rtkit, or add an rtprio limit (e.g. `@audio - rtprio 95` in `/etc/security/limits.d/`, user in group `audio`). |
| `SchedulingError` after starting (missed onset, clock changed, late callback) | The stimulus was dropped rather than played late. Reopen the stream; mark the trial invalid. |
| Silence on a `screen` device | The monitor must be on and connected (`no screen connected` in the list), with its own volume up. |

## How it works

- libasound (`libasound.so.2`) is loaded at runtime: no build dependency.
  Output is float32, else signed-32, else signed-16, converted natively.
- The worker thread gets real-time priority with `SCHED_FIFO` when allowed,
  otherwise `SCHED_RR` through rtkit, as PipeWire does; no sudo needed. rtkit
  requires capping the process `RLIMIT_RTTIME` at 200 ms. This cannot be undone
  within the process and only affects other code that busy-spins at real-time
  priority.
- Onsets are placed with a device-clock model: a line fitted to 2 s of
  (status-call time, frame being played) pairs. Single ALSA reports are noisy
  (USB timestamps lag by milliseconds); the fit is not. Reads interrupted for
  more than 200 µs and the first 20 ms after start, which USB reports ~1 ms
  early, are ignored; a disagreement over 5 ms rebuilds the model
  (`clock_resets`). `start()` returns once the model has data (~30 ms).
- Up to 32 stimuli can be queued; each period, every active one is added at
  its sample position. Overlaps are rejected at `schedule`; a stimulus whose
  onset can no longer be met is dropped alone (`MISSED`), while a stream fault
  ends all of them. Finished records stay readable (`wait(stimulus=id)`)
  until 32 newer stimuli reuse their slots.
- The output delay is about twice the buffer on USB devices (a buffer's worth of
  frames is in USB transfers) and about the buffer on PCI HDA.
- A missed deadline, underrun, partial write or invalid timestamp fails
  explicitly, without replay. `cancel()` stops further samples, not those
  already given to the device. All times are ALSA-reported estimates, not
  measured DAC output.

## Validation

Ubuntu 24.04, 20 CPU threads. Jitter was measured acoustically with a
HyperX Cloud Alpha Wireless USB headset (`hw:2,0`, S16 only): a 10 ms chirp was
scheduled, picked up by the headset's own microphone, captured on raw ALSA with
`CLOCK_MONOTONIC` timestamps and located by matched filter. Its arrival is a
constant ~68 ms after onset (radio links); what matters is the spread. The
measurement floor is 9 µs; the headset itself adds ±50 µs wander, occasional
~250 µs steps and rare radio dropouts. 40 trials per run.

**tachypy end to end**

| mode | jitter std | peak-to-peak |
|---|---|---|
| `timing_mode="software"` (default, via PipeWire) | 3.4 ms | 17.1 ms |
| `timing_mode="native"`, all defaults (5 ms buffer chosen) | 69 µs | 230 µs |
| `timing_mode="native"`, `latency=0.040` | 37 µs | 161 µs |

**What each change brought**

| build | result |
|---|---|
| before | every USB device rejected at the first status read |
| USB delay check fixed, onset from each status read | 399 µs std, 1.2 ms p2p |
| device-clock model | 37 µs std, 115 µs p2p (6 µs from the estimator itself) |
| startup reads ignored | stimulus right after `start()`: 450 µs late → within 12 µs |

**Buffer size under full CPU load** (20 busy processes), which set the
automatic `latency`:

| buffer | with real-time priority | without |
|---|---|---|
| 2 ms | fails at once (even idle) | — |
| 3 ms | 2 runs, 80/80 rendered | 2 runs, both failed |
| 5 ms | 2 runs, 80/80 | 4 runs, 2 failed |
| 10 ms | 2 runs, 80/80 | 4 runs, 1 failed |
| 20 ms | — | 3 runs, 120/120 |
| 40 ms | 2 runs, 80/80 | 3 runs, 120/120 |

Idle, every buffer from 3 to 40 ms rendered 40/40 with 29–104 µs jitter.

**Queue.** Twenty 10 ms chirps queued at once, 50 ms apart: 14 µs jitter, the
same as one at a time (13 µs). Two chirps queued back to back: measured seam
10 ms − 10 µs, under one sample. One at a time (schedule, wait, schedule), the
shortest onset-to-onset interval for 10 ms sounds on the ALC897 was 101 ms
with the former defaults (40 ms buffer, fixed 50 ms lead) and 25 ms with the
automatic ones; queued, there is no minimum. The
onboard ALC897 (`hw:CARD=PCH,DEV=0`, `s32_le`) passed the silent internal probe
(150 trials, no failure, placement within one sample), with an earlier build.

Not yet shown: absolute latency (needs an electrical reference, see
[electrical loopback](electrical-loopback.md)), a wired interface without the
headset's radio, and any comparison with PsychPortAudio.

### Running it yourself

```sh
python -m pip install -e . && python -m unittest discover -s tests
# silent internal check of sample placement
python examples/measure_scheduled_timing.py --device-id hw:CARD=PCH,DEV=0 \
  --channels 2 --trials 100 --output build/native.json
# physical check: chirps recorded back by a cable loopback or a headset mic
python examples/measure_loopback.py --device-id hw:2,0 --trials 40 --output build/loopback.json
```

`measure_loopback.py` needs numpy; use `--capture-id` to record on another
device, `--cpu-load` to load every core, and a low `--amplitude` if the
headset is worn. The unit tests run the real C worker against a simulated ALSA,
including a USB-like mode (noisy delay, stale timestamps) that checks the true
onset it produces.
