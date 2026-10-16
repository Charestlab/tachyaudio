# Changelog

All notable changes to `tachyaudio` will be documented in this file.

The project follows semantic versioning once releases begin. During `0.x`,
public APIs may still change while the backend design stabilizes.

## [Unreleased]

### Added

- Initial direct ALSA scheduled output with explicit hardware selection,
  monotonic status/delay estimates, float32/signed-16 hardware formats and
  simulated-worker tests. Ubuntu hardware validation remains pending; see
  `docs/ubuntu-scheduling.md`. Windows scheduling remains unavailable.

- Experimental macOS `ScheduledOutputStream` with device-clock sample placement,
  native deadline failures, cancellation, render timing estimates, and tests
  under Python contention. See `docs/native-scheduling.md`; physical timing and
  comparative PsychPortAudio performance have not been validated.
- A macOS scheduled-playback timing probe with native queue timestamps, raw
  trial data, and latency/jitter summaries under idle and Python-thread load.
  See `docs/timing-validation.md`; these measurements are not acoustic onset.

### Fixed

- Prefill finite playback before starting without blocking on clips larger than
  the ring buffer.
- Keep macOS output running after drain for stream reuse; include pending
  AudioQueue buffers in queue statistics and release the GIL during device start.
- Exclude deliberate idle/draining silence from output underrun counters.
- Account for a backend-buffer tail when draining Linux/Windows output.
- Add opt-in silent hardware playback regression tests:
  `TACHYAUDIO_TEST_HARDWARE=1 python -m unittest discover -s tests`.

## [0.2.0b2] - 2026-06-19

### Added

- Added Windows native backend support through vendored `miniaudio` using
  WASAPI.
- Added Windows CI test coverage and Windows wheel build artifacts.

### Changed

- Shared the miniaudio stream implementation across Linux and Windows with a
  small native lock/sleep portability layer.

## [0.2.0b1] - 2026-06-12

### Added

- Added the public `DuplexStream` API contract for backend-level full-duplex
  capture and playback.
- Added native macOS `DuplexStream` support backed by Core Audio input and
  output queues owned by one native stream object.
- Added native Linux `DuplexStream` support backed by one miniaudio duplex
  device callback.

### Changed

- Promoted package metadata from pre-alpha to beta.
- Kept wheel builds intentionally per-CPython-version rather than stable ABI.

## [0.2.0a3] - 2026-06-12

### Changed

- Refactored native ring-buffer and stream-stat helpers without changing public
  behavior.
- Added `hardware_latency` to stream statistics. macOS reports Core Audio
  device latency plus safety offset; Linux reports `None` until a reliable
  miniaudio latency value is available.
- Added blocking convenience helpers `OutputStream.write_all()` and
  `InputStream.read_exactly()`.

## [0.2.0a2] - 2026-06-10

### Changed

- Linux device enumeration now returns no devices instead of raising when
  miniaudio cannot initialize or enumerate an audio backend in headless
  containers.

## [0.2.0a1] - 2026-06-10

### Added

- Linux native backend using vendored `miniaudio`.
- Linux device enumeration through miniaudio.
- Linux native output playback with a ring-buffer-backed `OutputStream`.
- Linux native input capture with nonblocking `InputStream.read()`.
- Linux CI coverage for Python 3.10 through 3.14.
- Vendored miniaudio license attribution.

### Changed

- Linux backend initialization prefers PulseAudio, then falls back to ALSA.

### Known Limitations

- Linux support is new and has only been validated on a small set of Ubuntu
  audio configurations.
- Windows backend support is not implemented yet.
- The API is pre-alpha and may change before a stable release.

## [0.1.0] - 2026-06-10

### Added

- Initial Python package scaffold.
- macOS Core Audio device enumeration.
- macOS native output playback with a ring-buffer-backed `OutputStream`.
- Finite playback helper via `tachyaudio.play()`.
- Output stream lifecycle methods: `start()`, `stop()`, `drain()`, `flush()`,
  and `close()`.
- macOS native input capture with nonblocking `InputStream.read()`.
- Stream statistics for processed frames, underruns, overruns, queued frames,
  queued latency, and buffer size.
- Playback and capture examples.
- macOS CI and wheel workflow scaffolding for Python 3.10 through 3.14.

### Known Limitations

- Native backend support is currently macOS-only.
- Windows and Linux backends are not implemented yet.
- The API is pre-alpha and may change before the first PyPI release.
