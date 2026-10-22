"""One-command acoustic report of the audio system (speakers + microphone).

Installed as the ``tachyaudio-audio-report`` command. Plays short, near-inaudible
chirps through the speakers into the microphone and prints device-reported
speaker/microphone latencies, the native scheduler's device-clock precision, and
the end-to-end acoustic onset jitter (idle and under CPU load). The scheduler
precision is SNR-independent; the acoustic jitter is a physical cross-check and
is limited by the speakers/microphone. Latencies are device-reported, not an
externally calibrated absolute onset.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import platform
import sys
import threading
import time
from dataclasses import asdict
from pathlib import Path

try:
    import numpy as np
except ImportError:  # numpy is an optional extra for this diagnostic tool
    np = None

import tachyaudio as ta

RATE = 48_000
_READ_FRAMES = 480
# Windowed near-ultrasonic chirp for matched-filter detection.
_CHIRP_FRAMES = 1920
_CHIRP_F0, _CHIRP_F1 = 17_000.0, 20_000.0
_CHIRP_AMP = 0.18


class AudioReportError(RuntimeError):
    """A problem the user can act on (permissions, volume, no device)."""


def _burn():  # module-level so multiprocessing can spawn it
    v = 1
    while True:
        v = (v * 1664525 + 1013904223) & 0xFFFFFFFF


def _chirp() -> np.ndarray:
    t = np.arange(_CHIRP_FRAMES) / RATE
    k = (_CHIRP_F1 - _CHIRP_F0) / (_CHIRP_FRAMES / RATE)
    phase = 2 * np.pi * (_CHIRP_F0 * t + 0.5 * k * t * t)
    return (_CHIRP_AMP * np.hanning(_CHIRP_FRAMES) * np.sin(phase)).astype("float32")


def _default_device(kind: str) -> str:
    cands = [d for d in ta.list_devices() if d.kind.value in (kind, "duplex")]
    if not cands:
        raise AudioReportError(f"No {kind} device found.")
    return next((d for d in cands if d.is_default), cands[0]).id


def _device_name(device_id: str) -> str:
    return next((d.name for d in ta.list_devices() if d.id == device_id), device_id)


def _capture_peak(in_dev: str, seconds: float = 0.4) -> float:
    """Record ambient sound briefly; returns the peak amplitude (0.0 if silent)."""
    chunks = []
    try:
        with ta.InputStream(sample_rate=RATE, channels=1, device_id=in_dev) as cap:
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                data = cap.read(_READ_FRAMES)
                if len(data):
                    chunks.append(np.frombuffer(data, dtype="float32"))
                else:
                    time.sleep(0.001)
    except Exception as exc:
        raise AudioReportError(f"Could not open the microphone: {exc}") from exc
    audio = np.concatenate(chunks) if chunks else np.zeros(0, "float32")
    return float(np.max(np.abs(audio))) if len(audio) else 0.0


def _check_microphone(in_dev: str) -> None:
    """Fail early with platform-specific guidance if the microphone is silent/blocked."""
    if _capture_peak(in_dev) > 0.0:
        return
    if sys.platform == "darwin":
        how = (
            "grant Microphone access to your terminal/IDE in System Settings > "
            "Privacy & Security > Microphone (you may have seen a prompt)"
        )
    elif sys.platform.startswith("linux"):
        how = (
            "unmute the input (e.g. `alsamixer` > F4, or your sound settings), "
            "check the default input source and that your user is in the `audio` "
            "group; if running in a Flatpak/snap, grant it microphone access"
        )
    elif sys.platform.startswith("win"):
        how = "allow microphone access in Settings > Privacy & security > Microphone"
    else:
        how = "check your system's microphone permission and input settings"
    raise AudioReportError(
        f"The microphone returned only silence. {how}, make sure it is not muted "
        "or in use by another app, then re-run."
    )


def _detect(audio: np.ndarray, signal: np.ndarray, trials: int):
    nfft = 1 << (len(audio) + len(signal) - 1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(audio, nfft) * np.conj(np.fft.rfft(signal, nfft)), nfft)
    scores = np.abs(corr[: len(audio) - len(signal) + 1])
    working, picks, radius = scores.copy(), [], int(0.08 * RATE)
    for _ in range(trials):
        i = int(np.argmax(working))
        picks.append(i)
        working[max(0, i - radius) : i + radius + 1] = 0
    idx = np.array(sorted(picks))
    return idx, scores[idx]


def _measure(out_dev: str, in_dev: str, latency, trials: int, load: bool):
    signal = _chirp()
    rng = np.random.default_rng(731)
    spacing = np.cumsum(rng.uniform(0.13, 0.19, trials))
    spacing -= spacing[0]
    chunks, failures, stop = [], [], threading.Event()

    with ta.InputStream(sample_rate=RATE, channels=1, device_id=in_dev) as capture:

        def reader():
            try:
                while not stop.is_set():
                    data = capture.read(_READ_FRAMES)
                    chunks.append(
                        np.frombuffer(data, dtype="float32").copy()
                        if len(data)
                        else np.zeros(0, "float32")
                    )
                    if not len(data):
                        time.sleep(0.001)
            except Exception as exc:  # pragma: no cover - hardware dependent
                failures.append(str(exc))

        # Separate processes avoid starving capture through the GIL.
        burners = [mp.Process(target=_burn, daemon=True) for _ in range(4)] if load else []
        for b in burners:
            b.start()
        reader_thread = threading.Thread(target=reader, daemon=True)
        reader_thread.start()
        try:
            time.sleep(0.4)
            with ta.ScheduledOutputStream(
                sample_rate=RATE, channels=1, device_id=out_dev, latency=latency
            ) as stream:
                lead, status = stream.lead_time, stream.status()
                origin = time.monotonic() + 0.3
                targets = [origin + float(s) for s in spacing]
                ids = [stream.schedule(signal, when=t) for t in targets]
                results = [asdict(stream.wait(timeout=8, stimulus=i)) for i in ids]
                time.sleep(0.3)
            cap_stats = asdict(capture.stats())
        finally:
            stop.set()
            reader_thread.join(timeout=3)
            for b in burners:
                b.terminate()
            for b in burners:
                b.join(timeout=2)

    if failures:
        raise AudioReportError("Microphone capture failed: " + "; ".join(failures))
    audio = np.concatenate(chunks) if chunks else np.zeros(0, "float32")
    if len(audio) < len(signal) or not np.any(audio):
        raise AudioReportError("No microphone signal during the measurement; re-run.")

    idx, peaks = _detect(audio, signal, trials)
    arrivals = idx / RATE
    slope, intercept = np.polyfit(spacing, arrivals, 1)
    residual_us = (arrivals - (intercept + slope * spacing)) * 1e6
    spacing_err_us = (np.diff(arrivals) - np.diff(spacing)) * 1e6
    valid = bool(
        np.min(peaks) > 0.2 * np.median(peaks)
        and np.max(np.abs(spacing_err_us)) < 5000
        and not cap_stats["overruns"]
    )
    onset_err_us = [(r["predicted_onset_ns"] - r["requested_ns"]) / 1e3 for r in results]
    return {
        "condition": "cpu-load" if load else "idle",
        "detections_valid": valid,
        "jitter_std_us": float(np.std(residual_us)),
        "jitter_max_us": float(np.max(np.abs(residual_us))),
        "scheduler_onset_error_us_std": float(np.std(onset_err_us)),
        "scheduler_onset_error_us_max": float(np.max(np.abs(onset_err_us))),
        "output_delay_ms": status["output_delay_us"] / 1000.0,
        "presentation_latency_ms": status.get("presentation_latency_us", 0.0) / 1000.0,
        "lead_time_ms": lead * 1000.0,
        "period_frames": status["period_frames"],
        "input_hardware_latency_ms": cap_stats["hardware_latency"] * 1000.0,
        "capture_drift_ppm": float((slope - 1) * 1e6),
        "capture_peak": float(np.max(np.abs(audio))),
    }


def _line(label, value):
    return f"  {label:<34}{value}"


def _run(args) -> dict:
    out_dev = args.output_device or _default_device("output")
    in_dev = (
        None
        if (args.tune_only or args.immediate_only)
        else args.input_device or _default_device("input")
    )

    print("=" * 64)
    print(f"  Audio system report  |  tachyaudio {ta.__version__}")
    print("=" * 64)
    print(_line("Platform", platform.platform()))
    print(_line("Output (speaker)", f"{_device_name(out_dev)}  [{out_dev}]"))
    if in_dev is not None:
        print(_line("Input (microphone)", f"{_device_name(in_dev)}  [{in_dev}]"))
    print(_line("Sample rate / buffer", f"{RATE} Hz / {args.latency * 1000:.1f} ms requested"))
    immediate = None
    if not args.skip_lead_scan and (sys.platform == "darwin" or sys.platform.startswith("linux")):
        from tachyaudio._lead_probe import scan_immediate

        print("\n  -- Immediate playback: prepared-stream estimated delay --")
        immediate = scan_immediate(
            out_dev,
            RATE,
            buffers=(
                (0.000313, 0.0005, 0.001, 0.002, 0.005, args.latency)
                if sys.platform == "darwin"
                else (0.001, 0.002, 0.005, 0.01, 0.02, args.latency)
            ),
            trials=args.lead_trials,
            validation_trials=args.validation_trials,
            load_workers=args.load_workers,
            stimulus_ms=args.stimulus_ms,
        )
        print("  Frames / requested buffer   CPU workers   Median / p99 delay   Failures")
        for row in immediate["rows"]:
            delay = (
                f"{row['onset_median_ms']:.2f} / {row['onset_p99_ms']:.2f} ms"
                if "onset_p99_ms" in row
                else row.get("error", "unavailable")
            )
            print(
                f"  {row.get('initial_status', {}).get('period_frames', '?')} / "
                f"{row['latency_s'] * 1000:6.3f} ms  {row['load_workers']:>6}        "
                f"{delay}   {row['failures']}"
            )
        chosen = immediate["recommendation"]
        if chosen:
            print(
                f"  Validated immediate setting: latency={chosen['latency_s']:.6f}; "
                f"loaded p99={chosen['loaded_p99_ms']:.2f} ms."
            )
            print(
                "  Use tachypy Audio(timing_mode='native', latency=...) then prepare(), play(data)."
            )
        else:
            print("  No immediate setting passed independent validation.")
        if sys.platform == "darwin":
            print("  Strategy: first available Core Audio callback; no Python lead margin.")
        else:
            print("  Strategy: ALSA scheduled onset at now + default lead + 5 ms margin.")
            print("  This measures the current Linux play-now path, not Core Audio callback-ASAP.")
        print("  Use play(..., when=T) for a predetermined onset. Device-clock")
        print("  estimates are not physical measurements; validate your real workload.")
        print("  Startup is additional; Audio.play also includes array validation/conversion.")
    if args.immediate_only:
        return {
            "platform": platform.platform(),
            "tachyaudio": ta.__version__,
            "immediate_scan": immediate,
        }
    tuning = None
    if not args.skip_lead_scan:
        from tachyaudio._lead_probe import scan_lead_times

        print("\n  -- Minimum tested advance notice (silent scheduling tests) --")
        tuning = scan_lead_times(
            out_dev,
            RATE,
            buffers=(
                tuple(r["latency_s"] for r in immediate["rows"])
                if immediate is not None
                else (0.000313, 0.0005, 0.001, 0.002, 0.005, args.latency)
                if sys.platform == "darwin"
                else (0.001, 0.002, 0.005, args.latency)
            ),
            trials=args.lead_trials,
            validation_trials=args.validation_trials,
            load_workers=args.load_workers,
        )
        print("  Buffer     Default lead   Search minimum (before independent validation)")
        for buffer in sorted({r["latency_s"] for r in tuning["rows"]}):
            rows = [r for r in tuning["rows"] if r["latency_s"] == buffer]
            defaults = [r["default_lead_s"] for r in rows if "default_lead_s" in r]
            leads = {r["requested_lead_s"] for r in rows if r["requested_lead_s"] is not None}
            passed = [
                lead
                for lead in leads
                if all(
                    any(
                        r["requested_lead_s"] == lead
                        and r["load_workers"] == workers
                        and r["passed"]
                        for r in rows
                    )
                    for workers in (0, args.load_workers)
                )
            ]
            default = f"{max(defaults) * 1000:.2f} ms" if defaults else "unavailable"
            best = f"{min(passed) * 1000:.2f} ms" if passed else "none passed"
            print(f"  {buffer * 1000:5.1f} ms   {default:>12}   {best}")
        chosen = tuning["recommendation"]
        if chosen:
            print(
                f"  Recommended tested setting: latency={chosen['latency_s']:.6f}, "
                f"minimum_lead_time={chosen['lead_time_s']:.6f}"
            )
            print(
                f"  Passed {args.validation_trials} fresh trials per condition "
                f"(idle and {args.load_workers} CPU workers)."
            )
            print(
                "  Apply these keywords to ScheduledOutputStream or "
                "tachypy.Audio(timing_mode='native')."
            )
        else:
            print("  No setting passed independent validation; no recommendation.")
        failed = sum(not r["passed"] for r in tuning["rows"] + tuning["validation"])
        print(f"  Failed/incomplete test batches: {failed} (details retained in JSON).")
        print("  This is the shortest TESTED lead, not a hardware minimum or a safety guarantee.")
        print("  Revalidate with your real stimuli and workload. start() warms the device;")
        print("  lead_time is advance notice for onset, not a minimum gap between sounds.")
        print("  This lead applies to explicit deadlines, not macOS immediate playback.")
    if args.tune_only:
        return {
            "platform": platform.platform(),
            "tachyaudio": ta.__version__,
            "lead_scan": tuning,
            "immediate_scan": immediate,
        }
    print("  Checking microphone...")
    _check_microphone(in_dev)
    print("  Playing (near-inaudible) chirps into the microphone...")

    idle = _measure(out_dev, in_dev, args.latency, args.trials, load=False)
    loaded = _measure(out_dev, in_dev, args.latency, args.trials, load=True)

    print("\n  -- Device-reported latencies --")
    print(_line("Speaker output latency", f"{idle['output_delay_ms']:.2f} ms"))
    print(_line("  of which hardware (fixed)", f"{idle['presentation_latency_ms']:.2f} ms"))
    print(_line("Microphone capture latency", f"{idle['input_hardware_latency_ms']:.2f} ms"))

    print("\n  -- tachyaudio scheduler precision (device clock) --")
    print(
        _line(
            "Onset error vs requested",
            f"{idle['scheduler_onset_error_us_std']:.2f} us std, "
            f"{idle['scheduler_onset_error_us_max']:.2f} us max",
        )
    )
    print(_line("Advance notice (lead_time)", f"{idle['lead_time_ms']:.2f} ms"))
    pf = idle["period_frames"]
    print(_line("Device period", f"{pf} frames ({pf / RATE * 1000:.2f} ms)"))
    print(_line("Queue depth / back-to-back", "32 sounds / seamless"))

    print("\n  -- End-to-end acoustic jitter (speaker -> microphone) --")
    for name, m in (("idle", idle), ("under load", loaded)):
        flag = "" if m["detections_valid"] else "   [!] low confidence: raise output volume"
        val = f"{m['jitter_std_us']:.2f} us std, {m['jitter_max_us']:.2f} us max{flag}"
        print(_line(f"Onset jitter, {name}", val))

    print("\n  Scheduler onset error is the reliable precision figure. Acoustic jitter")
    print("  is a physical cross-check limited by the speakers/microphone, not an")
    print("  externally calibrated absolute onset.")
    print("=" * 64)
    return {
        "platform": platform.platform(),
        "tachyaudio": ta.__version__,
        "output_device": out_dev,
        "input_device": in_dev,
        "sample_rate": RATE,
        "latency_requested_s": args.latency,
        "trials": args.trials,
        "idle": idle,
        "cpu_load": loaded,
        "lead_scan": tuning,
        "immediate_scan": immediate,
    }


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--latency", type=float, default=0.001, help="device buffer in seconds (default: 1 ms)"
    )
    p.add_argument("--trials", type=int, default=16, help="chirps per condition (3..32)")
    p.add_argument("--output-device", default=None, help="speaker id (default: system default)")
    p.add_argument("--input-device", default=None, help="microphone id (default: system default)")
    p.add_argument("--json", type=Path, default=None, help="also write the raw report here")
    p.add_argument("--lead-trials", type=int, default=100, help="trials per search condition")
    p.add_argument(
        "--validation-trials", type=int, default=200, help="fresh trials per confirmation condition"
    )
    p.add_argument("--load-workers", type=int, default=4, help="CPU processes for lead validation")
    p.add_argument(
        "--skip-lead-scan", action="store_true", help="skip the silent lead/buffer search"
    )
    p.add_argument(
        "--tune-only", action="store_true", help="only test lead times; no microphone required"
    )
    p.add_argument(
        "--immediate-only",
        action="store_true",
        help="measure prepared play-now response on macOS/Linux; no microphone",
    )
    p.add_argument(
        "--stimulus-ms",
        type=float,
        default=1,
        help="silent stimulus duration for immediate tests (default: 1 ms)",
    )
    args = p.parse_args(argv)
    if sys.platform not in ("darwin", "linux"):
        p.error("native timing reports currently require macOS or Linux")
    if sys.platform.startswith("linux") and not (args.output_device or "").startswith("hw:"):
        p.error("Linux native reports require --output-device hw:CARD,DEVICE (see aplay -l)")
    if not 0 < args.stimulus_ms <= 10000:
        p.error("stimulus-ms must be positive and at most 10000")
    if (args.tune_only or args.immediate_only) and args.skip_lead_scan:
        p.error("--tune-only cannot be combined with --skip-lead-scan")
    if min(args.lead_trials, args.validation_trials, args.load_workers) < 1:
        p.error("lead-trials, validation-trials and load-workers must be positive")
    lower = 0.000001 if sys.platform == "darwin" else 0.001
    if not lower <= args.latency <= 1.0:
        p.error(f"latency must be between {lower} and 1.0 seconds")
    if np is None and not (args.tune_only or args.immediate_only):
        raise SystemExit("tachyaudio-audio-report needs numpy: pip install 'tachyaudio[report]'")
    if not 3 <= args.trials <= 32:
        p.error("trials must be 3..32")
    try:
        report = _run(args)
    except AudioReportError as error:
        raise SystemExit(f"\nAudio report could not run:\n  {error}") from None
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Raw report written to {args.json}")


if __name__ == "__main__":
    main()
