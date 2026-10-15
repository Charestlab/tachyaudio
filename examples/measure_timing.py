"""Measure scheduled submission to the macOS output queue, not acoustic onset."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import random
import statistics
import sys
import threading
import time
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "min": min(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "stddev": statistics.pstdev(values),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def trial_metrics(row: dict[str, int]) -> dict[str, float]:
    return {
        "scheduling_error_ms": (row["write_begin_ns"] - row["target_ns"]) / 1e6,
        "write_duration_ms": (row["write_end_ns"] - row["write_begin_ns"]) / 1e6,
        "write_to_copy_ms": (row["data_copy_ns"] - row["write_begin_ns"]) / 1e6,
        "target_to_enqueue_ms": (row["enqueue_end_ns"] - row["target_ns"]) / 1e6,
        "enqueue_duration_ms": (row["enqueue_end_ns"] - row["enqueue_begin_ns"]) / 1e6,
        "target_to_buffer_return_ms": (row["data_complete_ns"] - row["target_ns"]) / 1e6,
    }


def contend_for_gil(stop: threading.Event) -> None:
    value = 0
    while not stop.is_set():
        for _ in range(10000):
            value = (value + 1) % 1000003


def measure_case(args: argparse.Namespace, block_size: int, condition: str) -> dict:
    import numpy as np
    from tachypy import Audio

    player = Audio(
        sample_rate=args.sample_rate,
        channels=1,
        block_size=block_size,
        device_id=args.device_id,
        timeout=5,
    )
    stop_load = threading.Event()
    load_thread = None
    rows = []
    rng = random.Random(args.seed + block_size)
    try:
        player.prepare()
        stream = player._stream
        native = stream._handle._handle
        if not hasattr(native, "_timing"):
            raise RuntimeError("Build the patched macOS native backend before running this probe")
        before_ns = time.monotonic_ns()
        clock = native._timing()
        after_ns = time.monotonic_ns()
        if not before_ns <= clock["clock_now_ns"] <= after_ns:
            raise RuntimeError("Native and Python clocks do not share the same timebase")
        clock_bracket_ns = after_ns - before_ns
        original_write = stream.write
        payload = np.zeros(1, dtype=np.float32)
        actual_buffer_frames = stream.stats().buffer_size
        if condition == "python-thread":
            load_thread = threading.Thread(target=contend_for_gil, args=(stop_load,), daemon=True)
            load_thread.start()
        for index in range(args.trials + args.warmup):
            writes = []

            def measured_write(data, writes=writes):
                begin_ns = time.monotonic_ns()
                accepted = original_write(data)
                end_ns = time.monotonic_ns()
                writes.append((begin_ns, end_ns, accepted))
                return accepted

            stream.write = measured_write
            previous = native._timing()
            target_ns = time.monotonic_ns() + rng.randrange(20_000_000, 60_000_000)
            player.play(payload, when=target_ns / 1e9).result(timeout=10)
            timing = native._timing()
            if player._stream is not stream:
                raise RuntimeError("Probe requires the same prepared stream throughout")
            if len(writes) != 1 or writes[0][2] != 1:
                raise RuntimeError("Expected exactly one accepted sample per isolated trial")
            if timing["data_buffer_count"] != previous["data_buffer_count"] + 1:
                raise RuntimeError("Cannot associate the native queue event with this trial")
            if not writes[0][0] <= timing["data_copy_ns"] <= timing["enqueue_begin_ns"]:
                raise RuntimeError("Invalid native copy/submission timestamp order")
            if timing["data_complete_ns"] < timing["data_copy_ns"]:
                raise RuntimeError("Drain returned before the sample buffer was returned")
            if index >= args.warmup:
                rows.append({
                    "trial": index - args.warmup,
                    "target_ns": target_ns,
                    "write_begin_ns": writes[0][0],
                    "write_end_ns": writes[0][1],
                    **timing,
                })
        metrics = [trial_metrics(row) for row in rows]
        return {
            "condition": condition,
            "requested_buffer_frames": block_size,
            "actual_buffer_frames": actual_buffer_frames,
            "buffer_duration_ms": actual_buffer_frames / args.sample_rate * 1000,
            "clock_check_bracket_ns": clock_bracket_ns,
            "summary_ms": {
                name: summarize([row[name] for row in metrics]) for name in metrics[0]
            },
            "trials": rows,
        }
    finally:
        stop_load.set()
        if load_thread is not None:
            load_thread.join()
        player.close()


def markdown_report(report: dict) -> str:
    lines = [
        "# macOS scheduled audio queue timing",
        "",
        "One silent sample per trial, using a prepared tachypy.Audio stream.",
        "Native timestamps mark ring-buffer copying, AudioQueue enqueue, and buffer return.",
        "These are software observations, not DAC or speaker onset timestamps.",
        "Buffer return includes the whole buffer, even though only one sample was submitted.",
        "",
        f"Platform: {report['platform']}; Python: {report['python']}",
        f"Trials per case: {report['settings']['trials']}; "
        f"discarded warmups: {report['settings']['warmup']}",
        "",
        "All values below are milliseconds. Standard deviation describes observed jitter.",
        "",
        "| Load | Frames | Write lateness p95 | Enqueue median | Enqueue p95 | "
        "Enqueue p99 | Enqueue SD | Enqueue max |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for case in report["cases"]:
        metrics = case["summary_ms"]
        enqueue = metrics["target_to_enqueue_ms"]
        lines.append(
            f"| {case['condition']} | {case['actual_buffer_frames']} | "
            f"{metrics['scheduling_error_ms']['p95']:.3f} | {enqueue['median']:.3f} | "
            f"{enqueue['p95']:.3f} | {enqueue['p99']:.3f} | "
            f"{enqueue['stddev']:.3f} | {enqueue['max']:.3f} |"
        )
    lines.extend([
        "",
        "The JSON contains raw timestamps and additional metrics, including write duration,",
        "copy delay, enqueue call duration, and buffer return delay.",
        "A competing Python thread measures GIL contention, not general system saturation.",
        "Results are descriptive samples from this machine, not timing guarantees.",
    ])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trials", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--block-sizes", type=int, nargs="+", default=[128, 480, 1024])
    parser.add_argument("--conditions", nargs="+", choices=["idle", "python-thread"],
                        default=["idle", "python-thread"])
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--device-id")
    parser.add_argument("--seed", type=int, default=731)
    parser.add_argument("--output", type=Path, required=True, help="New JSON report path")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This probe currently requires the patched macOS backend")
    if args.trials < 2 or args.warmup < 0 or min(args.block_sizes) < 1 or args.sample_rate < 1:
        parser.error("Use at least two trials, nonnegative warmup, and positive sizes/rate")
    if args.output.exists() or args.output.with_suffix(".md").exists():
        parser.error("Output already exists; choose a new report path")

    import tachypy.audio

    import tachyaudio

    root = Path(__file__).resolve().parents[1]
    sources = [root / "src/tachyaudio/_native.c", Path(tachypy.audio.__file__)]
    report = {
        "platform": platform.platform(),
        "python": platform.python_version(),
        "tachyaudio_version": tachyaudio.__version__,
        "python_switch_interval_seconds": sys.getswitchinterval(),
        "settings": {**vars(args), "output": str(args.output)},
        "source_sha256": {
            str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources
        },
        "cases": [],
    }
    for condition in args.conditions:
        for block_size in args.block_sizes:
            print(f"Measuring {condition}, {block_size} frames...", flush=True)
            report["cases"].append(measure_case(args, block_size, condition))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    text = markdown_report(report)
    args.output.with_suffix(".md").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
