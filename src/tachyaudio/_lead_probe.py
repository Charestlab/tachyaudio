"""Bounded, silent scheduling experiments; results are not real-time guarantees."""

from __future__ import annotations

import math
import multiprocessing as mp
import random
import statistics
import sys
import time
from array import array

import tachyaudio as ta


def _burn(stop, ready):
    value = 1
    ready.set()
    while not stop.is_set():
        for _ in range(10000):
            value = (value * 1664525 + 1013904223) & 0xFFFFFFFF


def _submit_immediate(stream, signal):
    if sys.platform == "darwin":
        return stream.schedule_asap(signal)
    # Match tachypy's ALSA lead-plus-margin path.
    return stream.schedule(signal, when=time.monotonic() + stream.lead_time + 0.005)


def _probe(device, rate, latency, lead, trials, load_workers, *, immediate=False, stimulus_ms=1):
    stop = mp.Event()
    ready = [mp.Event() for _ in range(load_workers)]
    workers = [mp.Process(target=_burn, args=(stop, event), daemon=True) for event in ready]
    row = {
        "latency_s": latency,
        "requested_lead_s": lead,
        "trials_requested": trials,
        "load_workers": load_workers,
        "trials": [],
        "mode": "immediate" if immediate else "deadline",
        "immediate_strategy": ("callback" if sys.platform == "darwin" else "deadline_plus_5ms")
        if immediate
        else None,
        "stimulus_ms": stimulus_ms,
    }
    try:
        for worker in workers:
            worker.start()
        if not all(event.wait(timeout=10) for event in ready):
            raise RuntimeError("CPU load workers did not start")
        opening = time.monotonic()
        with ta.ScheduledOutputStream(
            sample_rate=rate, channels=1, device_id=device, latency=latency
        ) as stream:
            row["open_start_ms"] = (time.monotonic() - opening) * 1000
            row["default_lead_s"] = stream.lead_time
            row["initial_status"] = stream.status()
            # Bypass the policy guard to test rendering limits.
            if not immediate:
                stream.minimum_lead_time = 0
            lead = lead if lead is not None else row["default_lead_s"]
            row["tested_lead_s"] = lead
            signal = array("f", [0.0]) * max(1, round(rate * stimulus_ms / 1000))
            rng = random.Random(731)
            for _ in range(trials):
                time.sleep(rng.uniform(0, 0.002))
                target = time.monotonic() + lead
                before = time.monotonic()
                trial = {"submitted_lead_s": target - before}
                try:
                    ident = (
                        _submit_immediate(stream, signal)
                        if immediate
                        else stream.schedule(signal, when=target)
                    )
                    trial["submission_s"] = time.monotonic() - before
                    result = stream.wait(timeout=stimulus_ms / 1000 + 2, stimulus=ident)
                    if immediate:
                        trial.update(
                            success=True,
                            call_to_onset_s=result.predicted_onset_ns / 1e9 - before,
                            delivery_margin_s=(
                                result.predicted_onset_ns - result.render_complete_ns
                            )
                            / 1e9
                            - stream.status().get("presentation_latency_us", 0) / 1e6,
                        )
                    else:
                        trial.update(success=True, onset_error_s=result.predicted_error_seconds)
                except Exception as error:
                    trial.update(success=False, error=str(error))
                row["trials"].append(trial)
                if stream.failed:
                    break
            row["final_status"] = stream.status()
    except Exception as error:
        row["error"] = str(error)
    finally:
        stop.set()
        for worker in workers:
            if worker.pid is not None:
                worker.join(timeout=2)
                if worker.is_alive():
                    worker.terminate()
                    worker.join(timeout=2)
    row["failures"] = sum(not t["success"] for t in row["trials"])
    row["passed"] = not row.get("error") and len(row["trials"]) == trials and row["failures"] == 0
    if immediate:
        delays = sorted(t["call_to_onset_s"] for t in row["trials"] if t["success"])
        if delays:
            row["onset_median_ms"] = statistics.median(delays) * 1000
            row["onset_p99_ms"] = delays[math.ceil(len(delays) * 0.99) - 1] * 1000
            row["onset_max_ms"] = delays[-1] * 1000
            row["minimum_delivery_margin_ms"] = (
                min(t["delivery_margin_s"] for t in row["trials"] if t["success"]) * 1000
            )
    return row


def scan_lead_times(
    device,
    rate=48000,
    buffers=(0.001, 0.002, 0.005),
    trials=100,
    validation_trials=200,
    load_workers=4,
    progress=print,
):
    """Find the shortest passing tested lead, then independently validate it.

    Coarse search uses 1 ms steps, then 0.25 ms around the first failure. All failures remain
    in the result. A recommendation requires fresh idle AND loaded validation.
    No settings are persisted; streams restore their buffer where supported.
    """
    if trials < 1 or validation_trials < 1 or load_workers < 1:
        raise ValueError("trial counts and load_workers must be positive")
    rows, candidates = [], []
    for latency in sorted(set(buffers)):
        progress(f"  Testing {latency * 1000:g} ms buffer: idle + {load_workers} CPU workers...")
        baseline = _probe(device, rate, latency, None, trials, 0)
        rows.append(baseline)
        if not baseline["passed"]:
            continue
        default = baseline["default_lead_s"]
        # Start on a whole-millisecond boundary.
        top = math.ceil(default * 1000) / 1000

        def test_lead(lead, latency=latency):
            pair = [
                _probe(device, rate, latency, lead, trials, workers)
                for workers in (0, load_workers)
            ]
            rows.extend(pair)
            if all(r["passed"] for r in pair):
                candidates.append({"latency_s": latency, "lead_time_s": lead})
                return True
            return False

        # Bound the coarse search, then refine its failure boundary.
        previous_pass = None
        for step in range(32):
            lead = round(top - step * 0.001, 6)
            if lead <= 0:
                break
            if test_lead(lead):
                previous_pass = lead
            else:
                if previous_pass is not None:
                    for fraction in (0.00025, 0.0005, 0.00075):
                        test_lead(round(lead + fraction, 6))
                break
    validation, recommendation = [], None
    for candidate in sorted(candidates, key=lambda c: (c["lead_time_s"], c["latency_s"])):
        progress(f"  Confirming {candidate['lead_time_s'] * 1000:g} ms lead with fresh trials...")
        pair = [
            _probe(
                device,
                rate,
                candidate["latency_s"],
                candidate["lead_time_s"],
                validation_trials,
                workers,
            )
            for workers in (0, load_workers)
        ]
        validation.extend(pair)
        if all(r["passed"] for r in pair):
            recommendation = candidate
            break
    return {
        "sample_rate": rate,
        "device": device,
        "rows": rows,
        "validation": validation,
        "recommendation": recommendation,
        "search_step_ms": 0.25,
        "load_workers": load_workers,
        "limitation": "Shortest tested setting; not a hardware minimum or safety guarantee. "
        "Silent 1 ms stimuli; device-clock checks, no physical onset measurement. "
        "Revalidate with your stimulus sizes and application workload.",
    }


def scan_immediate(
    device,
    rate=48000,
    buffers=(0.000313, 0.0005, 0.001, 0.002, 0.005),
    trials=100,
    validation_trials=200,
    load_workers=4,
    stimulus_ms=1,
    progress=print,
):
    """Rank immediate modes by loaded p99 onset delay, then validate afresh."""
    if min(trials, validation_trials, load_workers, stimulus_ms) <= 0:
        raise ValueError("trial counts, workers and stimulus duration must be positive")
    device_info = {}
    try:
        with ta.ScheduledOutputStream(sample_rate=rate, channels=1, device_id=device) as stream:
            device_info = stream.status()
    except Exception as error:
        device_info = {"error": str(error)}
    minimum = device_info.get("minimum_buffer_frames", 0)
    if minimum:
        # Native configuration accepts integer microseconds then rounds UP to frames.
        minimum_seconds = math.floor(minimum * 1e6 / rate) / 1e6
        buffers = (*buffers, max(0.000001, minimum_seconds))
    rows, candidates, validation = [], [], []
    for latency in sorted(set(buffers)):
        progress(f"  Immediate playback: {latency * 1000:g} ms buffer...")
        pair = [
            _probe(
                device,
                rate,
                latency,
                None,
                trials,
                workers,
                immediate=True,
                stimulus_ms=stimulus_ms,
            )
            for workers in (0, load_workers)
        ]
        rows.extend(pair)
        if all(row["passed"] for row in pair):
            candidates.append((pair[1]["onset_p99_ms"], latency))
    recommendation = None
    for _, latency in sorted(candidates):
        progress(f"  Confirming immediate playback with {latency * 1000:g} ms buffer...")
        pair = [
            _probe(
                device,
                rate,
                latency,
                None,
                validation_trials,
                workers,
                immediate=True,
                stimulus_ms=stimulus_ms,
            )
            for workers in (0, load_workers)
        ]
        validation.extend(pair)
        if all(row["passed"] for row in pair):
            recommendation = {"latency_s": latency, "loaded_p99_ms": pair[1]["onset_p99_ms"]}
            break
    return {
        "rows": rows,
        "validation": validation,
        "recommendation": recommendation,
        "stimulus_ms": stimulus_ms,
        "device_info": device_info,
        "sample_rate": rate,
        "load_workers": load_workers,
        "limitation": "Device-clock estimated submission-to-onset, not physical onset. "
        "Callback phase affects delay; explicit deadlines are tested separately. "
        "Revalidate on your workload and stimulus sizes.",
    }
