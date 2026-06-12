# tachyaudio

`tachyaudio` is a low-level audio package intended to replace tachypy’s direct
dependency on `sounddevice`/PortAudio over time.

Status: pre-alpha. The native backend currently supports macOS through Core
Audio and Linux through vendored `miniaudio`. Windows support is not implemented
yet.

## Goals

- install from `pip` without requiring users to install PortAudio separately
- keep real-time audio work out of Python callbacks where possible
- expose explicit latency, underrun, and overrun diagnostics
- support playback, capture, duplex streams, and device enumeration
- keep tachypy’s public API stable while the backend evolves

## Non-goals

- full DAW-style audio graph support in the initial release
- replacing OS audio stacks such as Core Audio, WASAPI, ALSA, or PulseAudio
- exposing backend-specific details as the primary user API

## Initial API shape

```python
import tachyaudio as ta

devices = ta.list_devices()

stream = ta.OutputStream(sample_rate=48_000, channels=2)
stream.write(samples)
with stream:
    ...

stats = ta.play(samples, sample_rate=48_000, channels=2)
```

The native backend currently supports device enumeration, float32 output
playback, and nonblocking float32 input capture on macOS and Linux.

`OutputStream` is currently a continuous stream. Write audio before or during
playback; if the stream runs out of queued frames it outputs silence and counts
an underrun.

Use `tachyaudio.play()` for finite stimuli that should start, drain, and close
as a single operation.

See `examples/play_wav.py` for dependency-free playback of WAV files exported
from an editor such as Audacity. The repository example uses a 48 kHz float WAV
to avoid runtime resampling. Pass `--eq` to show a lightweight five-band
terminal meter during playback.

Use blocking helpers when callers need complete buffer transfer:

- `OutputStream.write_all(frames, timeout=None)`: wait until all frames are
  accepted by the output ring
- `InputStream.read_exactly(frame_count, timeout=None)`: wait until exactly the
  requested number of frames has been captured

Full-duplex capture/playback is exposed as `DuplexStream`. Native macOS and
Linux support is available.

Lifecycle semantics:

- `stop()`: stop playback without discarding queued frames
- `drain()`: wait for queued frames to play
- `flush()`: discard queued frames without closing the stream
- `close()`: stop playback and release native resources

`StreamStats` reports:

- `frames_processed`: frames consumed by the backend
- `underruns` / `overruns`: buffer starvation or rejected writes
- `hardware_latency`: backend-reported device latency in seconds when available
- `queued_frames`: frames currently waiting in the native ring
- `queued_latency`: queued ring duration in seconds
- `estimated_latency`: `queued_latency + hardware_latency` when hardware latency
  is available, otherwise queued latency
- `buffer_size`: native callback buffer size in frames

## Development

Supported Python versions:

- Python 3.10
- Python 3.11
- Python 3.12
- Python 3.13
- Python 3.14

Run the standard-library test suite:

```bash
python3 -m unittest discover -s tests
```

Play a short test tone:

```bash
PYTHONPATH=src python3 examples/play_tone.py
```

Capture a short input buffer and print its RMS level:

```bash
PYTHONPATH=src python3 examples/capture_level.py
```

`InputStream.read(frame_count)` is nonblocking and returns currently available
frames up to `frame_count`.

On macOS, a restricted sandbox may hide Core Audio devices. On Linux, sandboxed
processes may be unable to reach the user PipeWire/PulseAudio server. If
`tachyaudio.list_devices()` returns an empty tuple or only generic ALSA devices
in a sandboxed environment, verify from an unsandboxed terminal before debugging
the backend. Headless Linux containers may return no devices while still being
able to build and import the native extension.
