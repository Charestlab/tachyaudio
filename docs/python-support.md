# Python support policy

`tachyaudio` targets CPython 3.10 through 3.14.

## Compatibility rules

- Keep public Python code syntax-compatible with Python 3.10.
- Avoid runtime dependencies unless they materially improve packaging or audio
  correctness.
- Build and test native wheels for every supported CPython minor version.
- Treat Python 3.10 as the lower-bound type-checking target.
- Add version-specific code only behind narrow compatibility helpers.

## Wheel targets

Initial wheel scope:

- `cp310`
- `cp311`
- `cp312`
- `cp313`
- `cp314`

The native extension currently uses the regular CPython C API. That means wheel
builds are per-CPython-version. Moving to Python’s stable ABI can be evaluated
later, but only after the native API settles.

CI should run unit tests on every supported Python minor version before a wheel
is published.
