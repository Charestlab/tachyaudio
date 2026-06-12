# tachyaudio architecture

## Package boundary

`tachyaudio` owns low-level audio primitives. `tachypy` should not depend on
backend-specific concepts directly. Instead, tachypy should wrap `tachyaudio`
from `tachypy.audio` and provide experiment-oriented scheduling, stimulus
construction, and compatibility behavior.

## Backend boundary

The public Python API delegates to an internal backend protocol:

- `list_devices()`
- `open_output_stream(config)`
- `open_input_stream(config)`

The first production backend should be native. The current first slices expose
macOS Core Audio device enumeration and output playback directly so the build
and Python boundary can be validated. Playback uses preallocated Core Audio
queue buffers fed by a native ring buffer. A future cross-platform
implementation can move to vendored `miniaudio` once the desired callback,
buffering, and drain semantics are clearer.

Finite playback uses an explicit drain mode: partial final buffers are padded
with silence without counting as underruns, and callbacks stop refilling once
queued audio has drained.

Stream lifecycle methods have distinct responsibilities: `stop()` halts playback,
`drain()` waits for queued audio to finish, `flush()` discards queued ring-buffer
audio, and `close()` releases native resources.

Input capture mirrors output buffering in reverse: Core Audio callbacks write
captured float32 frames into a native ring buffer, and Python reads available
frames without invoking Python from the audio callback.

The core stream methods remain nonblocking: `write()` accepts as many frames as
the output ring can hold, and `read()` returns currently available captured
frames. Blocking helpers (`write_all()` and `read_exactly()`) are Python-level
conveniences layered on top of those primitives.

Full-duplex support is modeled as a backend-level `DuplexStream`, not as a
Python wrapper around one `OutputStream` and one `InputStream`. Backends should
use a single native duplex callback where available so capture and playback share
one scheduling clock. The current macOS implementation owns both native Core
Audio queues inside one native stream object; Linux duplex support remains next.

Stream statistics distinguish queue state from hardware behavior. `queued_frames`
and `queued_latency` describe the native ring buffer. `hardware_latency`
describes backend-reported device latency when available. `buffer_size`
describes the callback buffer size in frames. `estimated_latency` reports
`queued_latency + hardware_latency` when hardware latency is available, otherwise
queued latency.

## Real-time constraints

Audio callbacks must not call Python. Native backend code should own:

- device callback execution
- ring-buffer reads and writes
- underrun and overrun counters
- format conversion when needed
- stream start/stop synchronization

Python should submit buffers, inspect diagnostics, and control stream lifecycle.

## Packaging strategy

Support CPython 3.10 through 3.14. Keep the Python layer syntax-compatible with
3.10, and build wheels for each supported CPython minor version because native
extension modules are ABI-specific unless we intentionally move to the stable
ABI.

Ship wheels for supported platforms:

- macOS arm64/x86_64 using Core Audio
- Windows x86_64/arm64 using WASAPI
- Linux x86_64/aarch64 using the best available runtime backend

Source builds should remain possible, but routine users should not need to
install PortAudio or a compiler.

## Native backend milestone

The minimum useful native backend is:

1. device enumeration
2. output stream
3. input stream
4. frame counters
5. underrun/overrun counters
6. explicit latency configuration
7. wheel builds in CI

## macOS sandbox note

Core Audio device enumeration can return zero devices when run from a restricted
sandbox, even when the host Mac has audio hardware. Validate native enumeration
from an unsandboxed process before treating an empty list as a backend defect.
