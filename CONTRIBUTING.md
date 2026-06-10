# Contributing

`tachyaudio` is pre-alpha. The current implementation is intentionally narrow:
macOS Core Audio device enumeration, output playback, and input capture.

## Development setup

Use Python 3.10 through 3.14.

```bash
python -m pip install -e .
python -m unittest discover -s tests
```

On macOS, restricted sandboxes may hide Core Audio devices. Validate audio
examples from an unsandboxed terminal before treating empty device lists as bugs.

## Manual smoke tests

Playback:

```bash
PYTHONPATH=src python examples/play_tone.py
```

Capture:

```bash
PYTHONPATH=src python examples/capture_level.py
```

## Contribution rules

- Keep the public Python API compatible with Python 3.10.
- Keep native audio callbacks free of Python calls.
- Add or update tests for behavior changes.
- Keep platform-specific backend behavior behind the backend protocol.
- Avoid new runtime dependencies unless they materially improve correctness or
  packaging.

## Pull request checklist

- Unit tests pass locally.
- Relevant examples still run when the change affects audio behavior.
- README/docs are updated for public API changes.
- Native code handles allocation and shutdown failures explicitly.

