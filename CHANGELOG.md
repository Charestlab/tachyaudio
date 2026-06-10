# Changelog

All notable changes to `tachyaudio` will be documented in this file.

The project follows semantic versioning once releases begin. During `0.x`,
public APIs may still change while the backend design stabilizes.

## [Unreleased]

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

