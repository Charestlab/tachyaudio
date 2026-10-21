# Changelog

All notable changes to `tachyaudio` will be documented in this file.

The project follows semantic versioning once releases begin. During `0.x`,
public APIs may still change while the backend design stabilizes.

## [Unreleased]

### Changed

- Linux automatic buffer with real-time priority is now 2 ms on PCI cards
  (onboard HDA) and stays 5 ms on USB and other buses, detected from sysfs.
  `lead_time` on the onboard ALC897 drops from 8.25 to 4.5 ms: 300/300 stimuli
  at 48 and 192 kHz, idle and with every core busy (USB faults at 2 ms). See
  `docs/linux-scheduling.md`.
- CI: actions moved to Node 24 releases (checkout/setup-python/upload-artifact
  v6), tests run with `-X faulthandler`, jobs have timeouts, superseded runs
  are cancelled and workflows get read-only permissions.

### Fixed

- `close()` from another thread while `start()` was promoting the ALSA worker
  could join the thread whose handle `start()` was still using (crash or hang
  in a close-during-start stress test; now waits for `start()` to finish).
- `schedule()` raises `SchedulingError` for insufficient advance notice in all
  cases; a deadline that passed the Python check but not the native re-check
  after copying used to raise `ValueError`.
- Intermittent segfault of the test suite on CI: the simulated ALSA library
  read `TACHY_FAKE_*` environment variables from the worker thread while tests
  restored `os.environ`. Tests now configure it through a C hook, and timing
  tests are re-run when the simulated worker was demonstrably starved by the
  host (shared CI machines), failing otherwise with the longest worker gap.

### Added

- macOS HAL scheduling now supports 32 queued stimuli with ids, per-stimulus
  results/cancellation, independent missed-onset handling, automatic lead time
  from native output timestamps and ready-clock startup. Optional `latency`
  requests a shared HAL buffer size with conditional restoration on close.
  Core Audio diagnostics, clock/device-change faults, deterministic queue tests
  and an integrated-speaker/microphone interval probe accompany the change.
- ALSA simulated-worker tests compile on macOS again: Linux-only RTPRIO limit
  checks are guarded without changing the Linux implementation.

- Initial direct ALSA scheduled output with explicit hardware selection,
  monotonic status/delay estimates, float32/signed-16 hardware formats and
  simulated-worker tests. Ubuntu hardware validation remains pending; see
  `docs/linux-scheduling.md`. Windows scheduling remains unavailable.
- Linux ALSA `ScheduledOutputStream` now negotiates signed-32-bit interleaved
  output as an intermediate precision tier between float32 and signed-16,
  matching common HDA/Realtek codecs that reject float32 but support 32-bit
  containers. Confirmed on real Ubuntu hardware (`hw:CARD=PCH,DEV=0`, ALC897):
  negotiation now lands on `s32_le` instead of falling all the way back to
  `s16_le`. `status()['device_format']` reports which tier was selected.
- The ALSA scheduling worker thread now requests real-time priority on start:
  `SCHED_FIFO` directly when permitted, otherwise `SCHED_RR` through rtkit over
  D-Bus (libdbus loaded on demand), which works for an unprivileged Ubuntu
  desktop user. `status()['rt_priority']` and `status()['rt_method']` report
  the result. Measured on a USB headset under full CPU load, a 3 ms buffer
  rendered 80/80 scheduled stimuli with RT; without it one run glitched by
  milliseconds and another faulted at once.
- Linux `ScheduledOutputStream` chooses its ALSA buffer and required advance
  notice automatically: `latency=None` picks 5 ms when the worker can get
  real-time priority and 20 ms otherwise (the smallest sizes that never failed
  under full CPU load), and `minimum_lead_time=None` derives the advance
  notice from the measured device output delay (`stream.lead_time`; about
  12 ms instead of a fixed 50 ms on the tested USB device). Both remain
  overridable. `status()` adds `buffer_automatic`, `period_frames`,
  `output_delay_us`, `output_delay_max_us` and `device_channels`.
- Linux `ScheduledOutputStream` queues up to 32 stimuli: `schedule()` returns
  an id, `wait(stimulus=...)`, `wait_rendered(stimulus=...)` and
  `cancel(stimulus)` target one stimulus (default: latest for waits, all for
  cancel), and `failed` tells a stream fault from a dropped stimulus.
  Overlaps are still rejected; back-to-back stimuli play without a gap
  (measured seam under one sample). A missed onset now drops only that
  stimulus. macOS keeps one stimulus at a time. tachypy's native mode queues
  `play()` calls on one shared stream instead of rejecting them.
- On Linux, `ScheduledOutputStream` without a `hw:` device, or one that fails to
  open, now raises an error listing every ALSA playback device: stable id,
  short id, type (monitor speakers over HDMI/DisplayPort, computer jacks,
  digital, USB), whether a screen is connected and which process holds it.
  `tachyaudio.list_alsa_outputs()` returns the same data.
- `start()` now returns once the ALSA clock model has data (~30 ms) and ignores
  the first 20 ms of status reads, which USB devices report ~1 ms early. A
  stimulus scheduled right after `start()` previously landed ~450 µs late.
- ALSA onset placement now uses a least-squares device-clock model fitted to
  the last 2 s of status reads instead of trusting each read. On a USB device
  this reduced measured acoustic onset jitter from 399 µs to about 30-70 µs
  standard deviation (the remainder is the wireless headset's own drift). New
  `status()` diagnostics: `clock_observations`, `clock_ppm`,
  `clock_jitter_us` and `clock_resets`.
- Experimental macOS `ScheduledOutputStream` with device-clock sample placement,
  native deadline failures, cancellation, render timing estimates, and tests
  under Python contention. See `docs/native-scheduling.md`; physical timing and
  comparative PsychPortAudio performance have not been validated.
- A macOS scheduled-playback timing probe with native queue timestamps, raw
  trial data, and latency/jitter summaries under idle and Python-thread load.
  See `docs/timing-validation.md`; these measurements are not acoustic onset.

### Fixed

- Linux scheduled output now duplicates mono stimuli onto stereo-only devices,
  as documented and as on macOS; it previously failed to open them, including
  with tachypy's default `channels=1`. ALSA's messages about rejected trial
  formats are no longer printed during negotiation.
- ALSA scheduling no longer rejects USB audio devices: their reported delay
  includes frames in flight beyond the ring buffer, which the worker treated
  as a clock fault at its first status read.
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
