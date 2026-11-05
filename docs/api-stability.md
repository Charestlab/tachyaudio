# API stability from 1.0

TachyAudio 1.x follows semantic versioning for the documented public Python API.
Compatible functionality can be added in minor releases; fixes preserve public
call signatures, return types, and documented behavior. Incompatible public API
changes require a new major version. Deprecations are documented before removal.

The compatibility boundary includes the exported stream classes and their
configuration/statistics dataclasses, device descriptions and discovery,
backend selection, exception classes, scheduled stimulus ids and timing results,
and the documented tuning entry points. Names starting with an underscore and
the keys of diagnostic `ScheduledOutputStream.status()` dictionaries are internal
or provisional; diagnostic keys may be added or revised without a major bump.

## Supported behavior

- Ordinary float32 playback/capture and duplex streams use the platform backend.
- Native scheduled output uses Core Audio on macOS, explicit direct ALSA `hw:`
  devices on Linux, and shared-mode WASAPI on Windows. No resampling is done.
- Windows requires the endpoint's float32 mix rate and uses the shared-engine
  default period. Older Python monotonic clocks retain caller-side tick
  quantization, even though rendering uses QPC timestamps.
- Cancellation cannot recall samples already submitted to the device. Missed
  scheduled onsets are reported rather than silently shifted later.
- Hardware discovery and tuning retain their documented platform restrictions.
  The acoustic/latency report currently supports macOS and Linux.

`ScheduledTiming` contains device-clock estimates. Release 1.0 does not turn
these into externally calibrated physical onset measurements or establish
universal timing limits for every device. The setup and electrical-loopback
protocols remain applicable to experiments that need physical validation.

## Validation and coverage

Install test dependencies and run the same checks as CI from the repository root:

```sh
python -m pip install '.[test]'
python -X faulthandler -m coverage run -m unittest discover -s tests
python -m coverage report
```

All Python modules, including device enumeration, diagnostics and tuning, are
included in combined statement/branch coverage with a 90% floor. This percentage
does not measure C lines. The native renderer/worker tests exercise compiled C
against synthetic callbacks and a simulated ALSA ABI; platform wheel tests and
opt-in hardware tests provide separate validation. Hardware tests stay opt-in.
