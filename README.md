# TachyAudio

**Precise sound timing for Python experiments.**

TachyAudio lets you play sounds, record audio, and decide exactly when a sound
should start. It was created for [TachyPy](https://github.com/Charestlab/tachypy)
and for experiments where a few milliseconds can change the meaning of a result.
You can also use it on its own in any Python project.

## Why TachyAudio exists

Playing a sound is easy. Starting it at a precise, repeatable time—while Python
is also drawing stimuli, reading responses, or preparing the next trial—is harder.

TachyAudio lets you prepare a sound in advance and give it a start time. Its
compiled audio engine then handles playback using the sound device's clock.
Python does not need to wake up at the instant the sound begins.

That gives you:

- **Precise timing:** schedule sounds for a particular moment, including while
  Python is busy doing other work.
- **Reliable sequences:** queue up to 32 sounds, place them back to back, or let
  them overlap. Overlapping sounds are mixed automatically.
- **Visible failures:** a sound whose scheduled start can no longer be met is
  reported as a failure, so you can identify an invalid trial.
- **Playback and recording:** use a simple function for one sound, or keep a
  stream open for a whole experiment.
- **Straightforward installation:** macOS, Linux, and Windows support, with no
  separate PortAudio installation. The core package has no Python dependencies.

## How precise is it?

TachyAudio is built for exceptionally consistent sound timing. In the documented
Linux tests, scheduled starts landed **within one audio sample** on the device's
clock: less than **21 microseconds at 48 kHz**, or **6 microseconds at 192 kHz**.

A separate recording test through a USB wireless headset measured start-time
variation of **69 microseconds**, compared with **3.4 milliseconds** through the
ordinary system audio path—about **50 times less variation on that setup**.

The sample-placement figures are device-clock estimates; the headset result was
measured from recorded sound. Device choice, connection, and workload affect the
results. See the [measurements and test conditions](docs/linux-scheduling.md#validation)
for the details, including tests under heavy CPU load.

## Install

```bash
python -m pip install tachyaudio
```

Supports Python 3.10–3.14 on macOS, Linux, and Windows. NumPy arrays work too, but
NumPy is optional: the examples below use Python's built-in tools.

## Play your first sound

This creates a quiet, quarter-second tone and plays it through the default output.

```python
import math
from array import array

import tachyaudio as ta

sample_rate = 48000
frequency = 440
seconds = 0.25

tone = array("f", (
    0.05 * math.sin(2 * math.pi * frequency * i / sample_rate)
    for i in range(round(sample_rate * seconds))
))

ta.play(tone, sample_rate=sample_rate, channels=1)
```

`ta.play()` waits for playback to finish and releases the device. For precisely
scheduled starts, use `ScheduledOutputStream` below.

Audio buffers contain 32-bit floating-point samples, normally between `-1` and
`1`. `channels=1` means mono; stereo buffers contain alternating left and right
samples. Set `sample_rate` to the rate of your audio.

## Core classes

| Class | Use it to… |
|---|---|
| `ScheduledOutputStream` | Play sounds at chosen start times, with timing handled by the audio engine. The main choice for timing-sensitive experiments. |
| `OutputStream` | Keep an output open and supply audio as your program runs. |
| `InputStream` | Record from a microphone or another input. |
| `DuplexStream` | Play and record through one shared stream. |

`DeviceInfo` describes an available audio device. `ScheduledTiming` gives the
estimated timing of a scheduled sound. `StreamStats` reports playback/capture
counts and buffering problems. Configuration and full-duplex results also have
named types: `StreamConfig`, `DuplexStreamConfig`, and `DuplexStreamStats`.

## Schedule two sounds

This example reuses `tone` and `sample_rate` from the first example. The two
sounds start half a second apart, without Python needing to run at either start.

On **macOS and Windows**, `device_id=None` selects the default output. On
**Linux**, choose a free hardware output from `ta.list_alsa_outputs()` and put
its `device_id` in the example. Scheduled Linux playback needs that device to
be available for exclusive use.

```python
import time

# On Linux, replace None with an id from ta.list_alsa_outputs().
device_id = None

with ta.ScheduledOutputStream(
    sample_rate=sample_rate,
    channels=1,
    device_id=device_id,
) as audio:
    start = time.monotonic() + audio.lead_time + 0.1
    first = audio.schedule(tone, when=start)
    second = audio.schedule(tone, when=start + 0.5)

    timing = audio.wait(stimulus=first, timeout=3)
    audio.wait(stimulus=second, timeout=3)

print(f"Estimated first-start error: {timing.predicted_error_seconds * 1e6:.1f} µs")
```

`lead_time` is the advance notice the device needs. Schedule ahead of that
limit; keep the stream open across trials to avoid repeatedly opening the device.
Use `audio.cancel(stimulus)` to cancel a queued sound, or `audio.cancel()` for all
queued sounds. Samples already sent to the device cannot be recalled.

For scheduled playback, the device must support your sample rate. On Windows,
use the output's current sample rate, commonly 48 kHz, and leave `latency` unset.
Platform setup: [macOS](docs/native-scheduling.md),
[Linux](docs/linux-scheduling.md), [Windows](docs/windows-scheduling.md).

## Record one second

```python
import tachyaudio as ta

with ta.InputStream(sample_rate=48_000, channels=1) as microphone:
    recording = microphone.read_exactly(48_000, timeout=3)

print(f"Recorded {len(recording) // 4} mono samples")
```

`read_exactly()` waits for the requested number of samples. For a live input
loop, `read(frame_count)` returns whatever audio is available, up to that amount.

## Keep playback running

Reuse `tone` from the first example to send multiple buffers through one output.

```python
with ta.OutputStream(sample_rate=sample_rate, channels=1) as audio:
    audio.write_all(tone, timeout=3)
    audio.write_all(tone, timeout=3)
    if not audio.drain(timeout=3):
        raise TimeoutError("Playback did not finish in time")
```

`write_all()` waits until the whole buffer has been accepted. `drain()` waits
for queued audio to finish. This is useful for continuous playback; use
`ScheduledOutputStream` when the start times themselves matter.

## Play and record together

This also reuses `tone` and `sample_rate` from the first example.

```python
with ta.DuplexStream(
    sample_rate=sample_rate,
    input_channels=1,
    output_channels=1,
) as audio:
    audio.write_all(tone, timeout=3)
    recording = audio.read_exactly(len(tone), timeout=3)
```

## Choose a device

```python
import tachyaudio as ta

for device in ta.list_devices():
    print(device.id, device.name, device.kind.value, device.default_sample_rate)
```

Pass an output or input id as `device_id` to the corresponding stream. A
`DuplexStream` accepts separate `input_device_id` and `output_device_id` values.
For Linux scheduled output, use `ta.list_alsa_outputs()` instead.

## Go further

- [Play a WAV file](examples/play_wav.py), including an optional live level meter.
- [Capture and inspect microphone levels](examples/capture_level.py).
- [Schedule sounds and inspect timing](docs/native-scheduling.md).
- [Find low-latency settings for your machine](docs/native-scheduling.md#immediate-playback-and-tuning).
- [Validate physical onset with a wired loopback](docs/electrical-loopback.md).
- [Public API stability and testing](docs/api-stability.md).

TachyAudio is the audio engine behind [TachyPy](https://github.com/Charestlab/tachypy),
which combines sound with visual stimuli and response handling for experiments.
