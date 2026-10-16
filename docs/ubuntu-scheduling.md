# Ubuntu scheduled output — development handoff

Status: validated on real Ubuntu hardware (see "Validation results" below).
Windows scheduled output remains unimplemented. Ordinary playback is unchanged.

## Design and limitations

- The existing `ScheduledOutputStream` API selects a separate C extension on
  Linux. It dynamically loads the OS's `libasound.so.2`; no ALSA development
  headers or new Python dependencies are required. The runtime library is still
  necessary: this is not dependency-free.
- Explicit `hw:CARD,DEVICE` is required, without PulseAudio/PipeWire or plugin
  fallback. The desktop server may own the device; resolve that explicitly,
  preferably using a dedicated audio interface.
- Configures exact requested rate/channels and interleaved float32 where
  supported, otherwise signed-32 and finally signed-16 with native
  conversion/saturation. No resampling or automatic channel mapping. Other
  hardware formats remain unsupported. `status()['device_format']` identifies
  the selected format and quantization (`float32_le`, `s32_le`, or `s16_le`).
- The worker thread requests `SCHED_FIFO` real-time priority on `start()` as a
  best effort; `status()['rt_priority']` reports whether it was granted. An
  unprivileged account with no `RLIMIT_RTPRIO` (the default) silently keeps
  normal priority — the stream still works, but is more exposed to underruns
  under heavy system load. To grant it, add the running user to the `audio`
  group and a rule such as `@audio - rtprio 95` under `/etc/security/limits.d/`
  (the same mechanism Ubuntu Studio/JACK/PipeWire use), then log in again.
- Requests a 40 ms hardware buffer, primes silence and keeps a native pthread
  running. ALSA monotonic status timestamps plus queued-frame delay estimate the
  next written sample's presentation. This is driver-dependent, **not independently
  validated DAC/link timing**. Shares sample-offset calculation and state values
  with macOS, not its entire renderer.
- A mutex protects short state snapshots/nonblocking writes; the worker polls
  every millisecond. Python need not run at onset, but this is neither hard real
  time nor a lock-free callback. Hardware/load validation is required.
- Invalid timestamps, underrun/suspend, partial writes and missed render deadlines
  fail explicitly without automatic replay. Faulted streams require reopening.
  Cancelling prevents further submission, not playback of already queued samples.
  Closing drops the PCM and joins the worker. Completion remains estimated.

Timing basis: [ALSA PCM status/delay](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)
and [monotonic timestamp API](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html).

## On the Ubuntu laptop

Preserve local edits before switching branches:

```sh
git fetch origin
git switch dev_0.2.0b3
git pull --ff-only
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
python -m unittest discover -s tests
aplay -l
```

`aplay` is an optional diagnostic from `alsa-utils`, not a runtime dependency.
Replace the example card name below with an actual device from its output:

```sh
python examples/measure_scheduled_timing.py \
  --device-id 'hw:CARD=YourCard,DEV=0' --channels 2 \
  --trials 100 --output build/ubuntu-native-first.json
```

The probe is silent, uses 48 kHz and 100–130 ms advance notice, and records all
trial failures, device format and source hashes. Nonzero exit means setup or
trial failures. Retain outliers. Do not use the general hardware-suite environment
flag as Ubuntu acceptance: that suite currently contains macOS-specific tests.

Install the companion tachypy development branch, then use the same device:

```python
import time
import numpy as np
from tachypy.audio import Audio

audio = Audio(sample_rate=48000, channels=2, device_id="hw:CARD=YourCard,DEV=0",
              timing_mode="native", timeout=2)
try:
    audio.prepare()
    timing = audio.play(np.zeros((480, 2), dtype=np.float32),
                        when=time.monotonic() + 0.250).result()
    print(timing)
finally:
    audio.close()
```

## Validation results

Run on a real Ubuntu 24.04 lab machine, `hw:CARD=PCH,DEV=0` (HDA Intel PCH,
ALC897 analog output; the device only exposes `S16_LE`/`S32_LE`, no float, per
`aplay --dump-hw-params`), stereo, 48 kHz, pipewire-pulse running alongside:

1. Open, format/rate negotiation and repeated stream reuse: pass. Negotiation
   lands on `s32_le` (confirms the S32 tier added above is actually exercised
   on real hardware, not just in the simulated test).
2. `TACHYAUDIO_TEST_HARDWARE=1 python -m unittest discover -s tests`: all
   non-macOS-specific tests pass (45 ran, 8 skipped as macOS-only), including
   the drain/underrun-tail regression tests.
3. `examples/measure_scheduled_timing.py --device-id 'hw:CARD=PCH,DEV=0'
   --channels 2 --trials 50`, idle/python-thread/python-held conditions: 0
   failures, sample-placement residual p95 ≈ 19.5–20.4 us, max ≈ 20.3–20.7 us
   (sub-one-sample at 48 kHz, 20.833 us), render lead ≈ 30 ms. No regression
   after the S32 format change.
4. `rt_priority` is reported `0` (not granted) on this account, since
   `ulimit -r` is `0` by default — expected without the `limits.d` grant
   described above; the worker still runs correctly at normal priority.
5. Acoustic/click verification: not independently confirmed by ear or
   electrical loopback in this pass (no loopback cable available). However,
   the Linux/Windows miniaudio output path's `drain()` tail-accounting fix
   (see CHANGELOG "Account for a backend-buffer tail when draining
   Linux/Windows output") was verified structurally: the fix is present in
   this branch (`output_tail_frames` gates `drain()`, not just the software
   ring), and the two regression tests that exercise it
   (`test_drain_keeps_engine_running_and_idle_is_not_an_underrun`,
   `test_short_finite_clip_does_not_count_tail_silence_as_underruns`) pass
   against this real device. Follow `electrical-loopback.md` for an
   independent physical confirmation when a loopback cable is available.

## Remaining validation

1. Verify low-level audible waveform integrity and stereo routing, without subjects.
2. Run idle and CPU/graphics-load trials; increase counts after the first pass.
3. Follow `electrical-loopback.md` for physical measurements. Internal sample-grid
   residuals do not establish acoustic accuracy or TDT/PsychPortAudio equivalence.

## Automated evidence

`test_alsa_scheduling.py` compiles the actual C worker against a simulated ALSA ABI
on POSIX hosts. Covers reuse with Python held across onset, overlap rejection,
cancel/close, timestamp failures, partial writes, s32/s16 format negotiation,
the `rt_priority` status flag and invalid data. This is simulation, not a
hardware measurement, but real device discovery, format negotiation, timing
and load tolerance have now also been exercised on real Ubuntu hardware; see
"Validation results" above.
