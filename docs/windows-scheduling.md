# Windows native scheduling

Windows supports device-clock playback through `ScheduledOutputStream` and
TachyPy's explicit `Audio(timing_mode="native")`. The scheduler uses WASAPI
shared-mode rendering and estimates sample presentation from the stream's
playback position and its correlated QPC timestamp.

- Use the Windows endpoint's current float32 shared mix sample rate. Pass mono
  to duplicate it across the endpoint, or match the endpoint's channel count.
- Pass `device_id=None` for the default output, or use an output ID from
  `tachyaudio.list_devices()`. Leave `latency=None` to use the shared engine period.
- For high-resolution `time.monotonic()` deadlines, use CPython with a
  `QueryPerformanceCounter()` monotonic clock. Check
  `time.get_clock_info("monotonic")`. On versions that use `GetTickCount64()`,
  TachyAudio maps deadlines to QPC at startup, but the caller's deadlines
  retain the system tick's coarse resolution.

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

The hardware tests also exercise scheduled playback, overlap, cancellation,
explicit device selection, and playback while Python holds the GIL. Run the
placement probe to record predicted residuals and missed trials under load.

```powershell
.\.venv\Scripts\python.exe examples/measure_scheduled_timing.py --trials 100 --output build/wasapi-timing.json
```

Use [electrical loopback](electrical-loopback.md) to compare predictions with
physical onset. The probe's residual only measures sample selection on the
estimated WASAPI timeline.
