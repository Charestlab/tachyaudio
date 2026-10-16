# Ubuntu scheduled output — development handoff

Status: initial ALSA implementation; **physical Ubuntu validation pending**.
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
  supported, otherwise signed-16 with native conversion/saturation. No resampling
  or automatic channel mapping. Other hardware formats remain unsupported.
  `status()['device_format']` identifies the selected format and quantization.
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

## Tomorrow's validation

1. Check open, hardware format/rate, timestamps and repeated stream reuse.
2. Exercise pending/playing cancellation, long clips, shutdown and fault reporting.
3. Verify low-level audible waveform integrity and stereo routing, without subjects.
4. Run idle and CPU/graphics-load trials; increase counts after the first pass.
5. Follow `electrical-loopback.md` for physical measurements. Internal sample-grid
   residuals do not establish acoustic accuracy or TDT/PsychPortAudio equivalence.

## Automated evidence

`test_alsa_scheduling.py` compiles the actual C worker against a simulated ALSA ABI
on POSIX hosts. Covers reuse with Python held across onset, overlap rejection,
cancel/close, timestamp failures, partial writes, integer format and invalid data.
This is simulation, not a hardware measurement. Ubuntu CI also builds the real
extension. Real device discovery, format negotiation, timing and load tolerance
remain to be tested on the laptop.
