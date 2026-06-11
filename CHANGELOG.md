# Changelog

All notable changes to `tachyaudio` will be documented in this file.

The project follows semantic versioning once releases begin. During `0.x`,
public APIs may still change while the backend design stabilizes.

## [Unreleased]

### Changed

- Refactored native ring-buffer and stream-stat helpers without changing public
  behavior.

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
