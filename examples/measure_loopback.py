"""Measure Linux scheduled-onset jitter through a physical loopback.

Schedules a chirp with ScheduledOutputStream, records it on an ALSA capture
device (a cable from output to input, or a headset's mic picking up its own
speaker), timestamps capture on CLOCK_MONOTONIC and locates each arrival with a
matched filter. The constant path latency cancels; the spread is onset jitter.
Requires numpy and exclusive hw: access to both devices (stop PipeWire clients
such as the GNOME Sound panel that keep the capture device open).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import multiprocessing
import os
import random
import sys
import threading
import time

import numpy as np

import tachyaudio as ta

RATE = 48_000
alsa = ctypes.CDLL("libasound.so.2")


class Timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


class Capture:
    """Signed-16 capture with (sample index, monotonic ns) pairs per read."""

    def __init__(self, device: str, channels: int, seconds: float) -> None:
        self.pcm = ctypes.c_void_p()
        self._check(alsa.snd_pcm_open(ctypes.byref(self.pcm), device.encode(), 1, 0), "open")
        code = alsa.snd_pcm_set_params(self.pcm, 2, 3, channels, RATE, 0, 100_000)
        self._check(code, "set_params")
        params = ctypes.c_void_p()
        alsa.snd_pcm_sw_params_malloc(ctypes.byref(params))
        alsa.snd_pcm_sw_params_current(self.pcm, params)
        alsa.snd_pcm_sw_params_set_tstamp_mode(self.pcm, params, 1)
        alsa.snd_pcm_sw_params_set_tstamp_type(self.pcm, params, 1)
        self._check(alsa.snd_pcm_sw_params(self.pcm, params), "sw_params")
        alsa.snd_pcm_sw_params_free(params)
        self.channels = channels
        self.data = np.zeros(int(seconds * RATE) * channels, dtype=np.int16)
        self.total = 0
        self.stamps: list[tuple[int, int]] = []
        self.overruns = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    @staticmethod
    def _check(code: int, what: str) -> None:
        if code < 0:
            raise RuntimeError(f"ALSA capture {what} failed: {alsa.snd_strerror(code).decode()}")

    def start(self) -> None:
        self._check(alsa.snd_pcm_start(self.pcm), "start")
        self._thread.start()

    def _run(self) -> None:
        chunk = 240
        buffer = (ctypes.c_int16 * (chunk * self.channels))()
        available, stamp = ctypes.c_ulong(), Timespec()
        while not self._stop.is_set() and (self.total + chunk) * self.channels <= len(self.data):
            count = alsa.snd_pcm_readi(self.pcm, buffer, chunk)
            if count < 0:
                self.overruns += 1
                alsa.snd_pcm_recover(self.pcm, count, 1)
                continue
            begin = self.total * self.channels
            self.data[begin:begin + count * self.channels] = np.frombuffer(
                buffer, dtype=np.int16, count=count * self.channels)
            self.total += count
            if alsa.snd_pcm_htimestamp(self.pcm, ctypes.byref(available), ctypes.byref(stamp)) == 0:
                self.stamps.append((self.total + available.value,
                                    stamp.tv_sec * 1_000_000_000 + stamp.tv_nsec))

    def close(self) -> tuple[np.ndarray, float, float]:
        """Return the first channel and a least-squares index -> ns clock map."""
        self._stop.set()
        self._thread.join()
        alsa.snd_pcm_close(self.pcm)
        pairs = np.array(self.stamps[50:], dtype=np.float64)
        slope, intercept = np.polyfit(pairs[:, 0], pairs[:, 1], 1)
        mono = self.data[:self.total * self.channels:self.channels].astype(np.float32) / 32768
        return mono, intercept, slope


def chirp(amplitude: float) -> np.ndarray:
    count = RATE // 100
    t = np.arange(count) / RATE
    sweep = np.sin(2 * np.pi * (1000 * t + 0.5 * 500_000 * t * t)) * np.hanning(count)
    return (sweep * amplitude).astype(np.float32)


def locate(signal: np.ndarray, template: np.ndarray, center: float,
           window: tuple[float, float]) -> tuple[float | None, float]:
    """Sub-sample arrival index of template near center, and peak/median ratio."""
    lo = max(int(center + window[0] * RATE), 0)
    segment = signal[lo:int(center + window[1] * RATE) + len(template)]
    if len(segment) < 2 * len(template):
        return None, 0.0
    size = 1 << int(np.ceil(np.log2(len(segment) + len(template))))
    spectrum = np.fft.rfft(segment, size) * np.conj(np.fft.rfft(template, size))
    analytic = np.zeros(size, dtype=complex)  # envelope of the correlation
    analytic[0] = spectrum[0]
    analytic[1:size // 2] = 2 * spectrum[1:size // 2]
    analytic[size // 2] = spectrum[size // 2]
    envelope = np.abs(np.fft.ifft(analytic))[:len(segment) - len(template) + 1]
    peak = int(np.argmax(envelope))
    fraction = 0.0
    if 0 < peak < len(envelope) - 1:
        left, mid, right = envelope[peak - 1:peak + 2]
        fraction = 0.5 * (left - right) / (left - 2 * mid + right)
    return lo + peak + fraction, float(envelope[peak] / (np.median(envelope) + 1e-12))


def spin() -> None:
    while True:
        pass


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", required=True, help="playback hw: device")
    parser.add_argument("--capture-id", help="capture hw: device (default: same as playback)")
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--capture-channels", type=int, default=1)
    parser.add_argument("--latency", type=float, help="ALSA buffer, seconds; default: automatic")
    parser.add_argument("--minimum-lead-time", type=float, help="default: automatic")
    parser.add_argument("--trials", type=int, default=40)
    parser.add_argument("--amplitude", type=float, default=0.5, help="keep low when worn")
    parser.add_argument("--cpu-load", action="store_true", help="busy-loop every CPU meanwhile")
    parser.add_argument("--max-latency", type=float, default=0.25,
                        help="search window after onset, seconds")
    parser.add_argument("--output", help="write per-trial JSON here")
    args = parser.parse_args()
    template = chirp(args.amplitude)
    frames = np.repeat(template[:, None], args.channels, axis=1).ravel()
    capture = Capture(args.capture_id or args.device_id, args.capture_channels,
                      args.trials * 0.6 + 10)
    capture.start()
    load = [multiprocessing.Process(target=spin, daemon=True)
            for _ in range(os.cpu_count() or 1)] if args.cpu_load else []
    for process in load:
        process.start()
    rows = []
    rng = random.Random(1)
    try:
        time.sleep(0.5)
        with ta.ScheduledOutputStream(sample_rate=RATE, channels=args.channels,
                                      device_id=args.device_id, latency=args.latency,
                                      minimum_lead_time=args.minimum_lead_time) as stream:
            for trial in range(args.trials):
                when = time.monotonic() + stream.lead_time + 0.005 + rng.uniform(0, 0.020)
                try:
                    stream.schedule(frames, when=when)
                    onset = stream.wait(timeout=3).predicted_onset_ns
                    rows.append({"trial": trial, "onset_ns": onset})
                except (ta.SchedulingError, ValueError, TimeoutError) as error:
                    rows.append({"trial": trial, "error": str(error)})
                    break
                time.sleep(rng.uniform(0.35, 0.45))
            status = stream.status()
            lead_time = stream.lead_time
    finally:
        for process in load:
            process.terminate()
        time.sleep(args.max_latency + 0.1)
        signal, intercept, slope = capture.close()
    latencies = []
    for row in rows:
        if "onset_ns" in row:
            index, ratio = locate(signal, template, (row["onset_ns"] - intercept) / slope,
                                  (-0.010, args.max_latency))
            row["snr"] = ratio
            if index is not None and ratio > 8:
                row["latency_ms"] = (intercept + slope * index - row["onset_ns"]) / 1e6
                latencies.append(row["latency_ms"])
    values = np.array(latencies)
    if len(values):
        values = values[np.abs(values - np.median(values)) < 2.0]
    summary = {
        "rendered": sum("onset_ns" in row for row in rows), "trials": args.trials,
        "detected": int(len(values)), "capture_overruns": capture.overruns,
        "buffer_frames": status["buffer_frames"], "buffer_automatic": status["buffer_automatic"],
        "lead_time_ms": lead_time * 1000,
        "output_delay_ms": status["output_delay_us"] / 1000,
        "rt_method": status["rt_method"], "clock_resets": status["clock_resets"],
        "latency_ms_mean": float(values.mean()) if len(values) else None,
        "jitter_us_std": float(values.std() * 1000) if len(values) else None,
        "jitter_us_p2p": float(np.ptp(values) * 1000) if len(values) else None,
        "errors": [row["error"] for row in rows if "error" in row],
    }
    print(json.dumps(summary, indent=2))
    if args.output:
        with open(args.output, "w") as handle:
            report = {"arguments": vars(args), "summary": summary, "trials": rows}
            json.dump(report, handle, indent=2)
    sys.exit(0 if summary["rendered"] == args.trials and summary["detected"] else 1)


if __name__ == "__main__":
    main()
