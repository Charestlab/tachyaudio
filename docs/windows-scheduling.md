# Windows native scheduling development

Tachyaudio's existing Windows backend uses WASAPI through miniaudio.
This procedure verifies ordinary playback before developing the
device-clock backend for `ScheduledOutputStream`.

## Build from source

Install 64-bit CPython 3.10 or later and the Visual Studio C++ build tools
with a Windows SDK. Run these commands from the repository root in PowerShell.

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e '.[dev]'
.\.venv\Scripts\python.exe -X faulthandler -m unittest discover -s tests
.\.venv\Scripts\python.exe -c "import tachyaudio as ta; print(ta.get_backend().name); print(ta.list_devices())"
```

If pip cannot write to its temporary directory, set `TEMP` and `TMP` to a
writable directory inside the checkout for the install.

## Verify on hardware

Run this from an interactive Windows session with a working playback device.
The playback tests use silence and exercise prefill, drain, and clips that span
more than one ring buffer.

```powershell
$env:TACHYAUDIO_TEST_HARDWARE = '1'
try {
    .\.venv\Scripts\python.exe -X faulthandler -m unittest discover -s tests -p test_native_playback.py -v
} finally {
    Remove-Item Env:TACHYAUDIO_TEST_HARDWARE
}
```

When an input endpoint is available, run
`.\.venv\Scripts\python.exe examples/capture_level.py` and check that it
reports captured frames.

## Scheduler validation

The Windows scheduler should use the WASAPI device clock to place finite
stimuli against Python monotonic deadlines. Exercise absolute onset, overlap
mixing, cancellation, missed deadlines, and playback while Python holds the
GIL. Compare predicted onset with a wired loopback measurement before making
claims about physical timing. See [native scheduling](native-scheduling.md)
for the existing API and [electrical loopback](electrical-loopback.md) for the
measurement setup.
