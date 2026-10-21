"""Acoustic interval validation using Mac speakers and microphone.

Measures detected arrival spacing and residual jitter after fitting capture
clock drift; it cannot measure absolute DAC onset (InputStream has no capture
host timestamps). Records only summary/detection data, not microphone audio.
"""
from __future__ import annotations

import argparse
import json
import platform
import threading
import time
from dataclasses import asdict
from pathlib import Path

import numpy as np
import tachyaudio as ta


def run(trials=16, latency=None, load=False):
    rate = 48000
    rng = np.random.default_rng(731)
    times = np.cumsum(rng.uniform(.13, .19, trials))
    times -= times[0]
    t = np.arange(480) / rate
    signal = (.06 * np.hanning(480) * np.sin(2 * np.pi * (1200*t + 180000*t*t))).astype('float32')
    chunks, failures = [], []
    stop = threading.Event()
    with ta.InputStream(sample_rate=rate, channels=1, device_id='BuiltInMicrophoneDevice') as capture:
        def read():
            try:
                while not stop.is_set():
                    data = capture.read(480)
                    if len(data):
                        chunks.append(np.frombuffer(data, dtype='float32').copy())
                    else:
                        time.sleep(.001)
            except Exception as exc:
                failures.append(str(exc))
        reader = threading.Thread(target=read)
        reader.start()
        busy = None
        def contend():
            value = 1
            while not stop.is_set():
                value = (value * 1664525 + 1013904223) & 0xffffffff
        try:
            # Let capture settle before scheduling the first chirp.
            time.sleep(.4)
            with ta.ScheduledOutputStream(sample_rate=rate, channels=1,
                    device_id='BuiltInSpeakerDevice', latency=latency) as stream:
                initial = stream.status()
                lead = stream.lead_time
                if load:
                    busy = threading.Thread(target=contend)
                    busy.start()
                origin = time.monotonic() + .3
                targets = [origin + float(value) for value in times]
                ids = [stream.schedule(signal, when=target) for target in targets]
                results = [asdict(stream.wait(timeout=8, stimulus=ident)) for ident in ids]
                final = stream.status()
                time.sleep(.3)
        finally:
            stop.set()
            reader.join(timeout=3)
            if busy:
                busy.join(timeout=3)
        capture_stats = asdict(capture.stats())
    if failures:
        raise RuntimeError('; '.join(failures))
    audio = np.concatenate(chunks) if chunks else np.zeros(0)
    if len(audio) < len(signal) or not np.any(audio):
        raise RuntimeError('No microphone signal: check microphone permission and system output volume')
    # FFT matched filter; local peaks separated by >80 ms exclude chirp echoes.
    nfft = 1 << (len(audio) + len(signal) - 1).bit_length()
    correlation = np.fft.irfft(np.fft.rfft(audio, nfft) * np.conj(np.fft.rfft(signal, nfft)), nfft)
    scores = np.abs(correlation[:len(audio) - len(signal) + 1])
    candidates = []
    working = scores.copy()
    radius = int(.08 * rate)
    for _ in range(trials):
        index = int(np.argmax(working))
        candidates.append(index)
        working[max(0, index-radius):index+radius+1] = 0
    indices = np.array(sorted(candidates))
    arrivals = indices / rate
    slope, intercept = np.polyfit(times, arrivals, 1)
    residual_us = (arrivals - (intercept + slope * times)) * 1e6
    spacing_error_us = (np.diff(arrivals) - np.diff(times)) * 1e6
    peaks = scores[indices]
    # Reject bad detections rather than reporting meaningless precision.
    detected = bool(np.min(peaks) > .2 * np.median(peaks) and
                    np.max(np.abs(spacing_error_us)) < 5000 and not capture_stats['overruns'])
    return {'platform': platform.platform(), 'tachyaudio': ta.__version__,
        'condition': 'python-thread' if load else 'idle', 'latency_requested': latency,
        'trials': trials, 'lead_time_s': lead, 'initial_status': initial, 'final_status': final,
        'capture_stats': capture_stats, 'detections_valid': detected,
        'capture_peak': float(np.max(np.abs(audio))), 'arrival_indices': indices.tolist(),
        'correlation_peaks': peaks.tolist(), 'requested_times': targets, 'native_results': results,
        'capture_clock_drift_ppm': float((slope-1)*1e6),
        'residual_jitter_us': residual_us.tolist(), 'residual_std_us': float(np.std(residual_us)),
        'residual_max_abs_us': float(np.max(np.abs(residual_us))),
        'spacing_error_us': spacing_error_us.tolist(),
        'limitation': 'Acoustic intervals only; constant latency and linear capture clock drift removed. No absolute onset measurement.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials', type=int, default=16)
    parser.add_argument('--latency', type=float)
    parser.add_argument('--load', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 3 <= args.trials <= 32:
        parser.error('trials must be 3..32')
    if args.output.exists():
        parser.error('output already exists')
    report = run(args.trials, args.latency, args.load)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:report[k] for k in ('detections_valid','residual_std_us',
        'residual_max_abs_us','capture_clock_drift_ppm','capture_peak','lead_time_s')}))
    if not report['detections_valid']:
        raise SystemExit('Microphone detections failed quality checks; do not interpret jitter')


if __name__ == '__main__':
    main()
