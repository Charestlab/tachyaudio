"""Validate native scheduled rendering; predictions are not measured DAC onset."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import random
import sys
import threading
import time
from array import array
from dataclasses import asdict
from pathlib import Path

from measure_timing import contend_for_gil, summarize

import tachyaudio as ta


def measure(trials: int, condition: str, seed: int, device_id: str | None,
            channels: int = 1) -> dict:
    rows = []
    stop = threading.Event()
    worker = None
    rng = random.Random(seed)
    with ta.ScheduledOutputStream(channels=channels, device_id=device_id) as stream:
        if condition == "python-thread":
            worker = threading.Thread(target=contend_for_gil, args=(stop,), daemon=True)
            worker.start()
        try:
            for index in range(trials):
                target = time.monotonic() + rng.uniform(0.100, 0.130)
                row = {"trial": index, "target_ns": round(target * 1e9)}
                try:
                    stream.schedule(array("f", [0.0] * channels), when=target)
                    if condition == "python-held":
                        previous = sys.getswitchinterval()
                        try:
                            sys.setswitchinterval(0.5)
                            while time.monotonic() < target + 0.025:
                                pass
                        finally:
                            sys.setswitchinterval(previous)
                    result = stream.wait_rendered(timeout=2)
                    row.update(asdict(result))
                    row["predicted_placement_error_us"] = result.predicted_error_seconds * 1e6
                    row["render_lead_ms"] = (
                        result.predicted_onset_ns - result.render_complete_ns
                    ) / 1e6
                    row["status"] = "rendered"
                except (ta.SchedulingError, ValueError, RuntimeError, TimeoutError) as error:
                    row["status"] = "failed"
                    row["error"] = str(error)
                    row["native_status"] = stream.status()
                rows.append(row)
        finally:
            stop.set()
            if worker is not None:
                worker.join()
        device_status = stream.status()
    succeeded = [row for row in rows if row["status"] == "rendered"]
    return {
        "condition": condition,
        "trials": rows,
        "rendered": len(succeeded),
        "failed": len(rows) - len(succeeded),
        "device_buffer_frames": device_status["buffer_frames"],
        "sample_rate": device_status["sample_rate"],
        "device_status": device_status,
        "predicted_placement_error_us": summarize([
            row["predicted_placement_error_us"] for row in succeeded
        ]) if succeeded else None,
        "render_lead_ms": summarize([
            row["render_lead_ms"] for row in succeeded
        ]) if succeeded else None,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trials", type=int, default=100)
    parser.add_argument("--conditions", nargs="+", default=["idle", "python-thread", "python-held"],
                        choices=["idle", "python-thread", "python-held"])
    parser.add_argument("--seed", type=int, default=731)
    parser.add_argument("--device-id")
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.trials < 2 or args.channels < 1:
        parser.error("Use at least two trials and a positive channel count")
    if args.output.exists() or args.output.with_suffix(".md").exists():
        parser.error("Output exists; choose a new report path")
    source_root = Path(__file__).resolve().parents[1] / "src/tachyaudio"
    source = source_root / (
        "_scheduled_alsa.c" if sys.platform.startswith("linux") else "_scheduled_coreaudio.h"
    )
    report = {
        "platform": platform.platform(),
        "python": platform.python_version(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "common_sha256": hashlib.sha256(
            (source_root / "_scheduled_common.h").read_bytes()
        ).hexdigest(),
        "device_id": args.device_id,
        "channels": args.channels,
        "note": "Sample-placement residual is derived from the device clock, not external onset.",
        "cases": [],
    }
    for condition in args.conditions:
        print(f"Measuring native scheduling: {condition}...", flush=True)
        report["cases"].append(measure(
            args.trials, condition, args.seed, args.device_id, args.channels
        ))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    lines = [
        "# Native scheduled playback estimates", "",
        "These are device-clock sample-placement estimates, NOT measured physical onset errors.",
        "Stimuli were submitted 100–130 ms in advance. Python does not run the onset scheduler.",
        "No external loopback or PsychPortAudio comparison was performed.", "",
        "| Condition | Rendered | Failed | Placement residual p95 (us) | "
        "Placement residual max (us) | Minimum render lead (ms) |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for case in report["cases"]:
        residual = case["predicted_placement_error_us"]
        lead = case["render_lead_ms"]
        if residual is None:
            lines.append(f"| {case['condition']} | 0 | {case['failed']} | n/a | n/a | n/a |")
        else:
            lines.append(
                f"| {case['condition']} | {case['rendered']} | {case['failed']} | "
                f"{residual['p95']:.3f} | {residual['max']:.3f} | {lead['min']:.3f} |"
            )
    lines.extend([
        "", "Placement residual is expected to be below one sample (20.833 us at 48 kHz)",
        "by construction. It validates sample selection, not the device timestamp's accuracy.",
        "Render lead is how early the native callback finished supplying the first sample relative",
        "to its predicted presentation. A positive margin shows Python wake-up was unnecessary.",
        "python-held runs a Python busy loop with a 500 ms thread-switch interval across onset.",
    ])
    text = "\n".join(lines) + "\n"
    args.output.with_suffix(".md").write_text(text)
    print(text)
    if any(case["failed"] for case in report["cases"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
