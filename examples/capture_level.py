from __future__ import annotations

import argparse
import math
import time
from array import array

import tachyaudio as ta


def rms_level(samples: array) -> float:
    if not samples:
        return 0.0
    total = 0.0
    for sample in samples:
        total += sample * sample
    return math.sqrt(total / len(samples))


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture a short input buffer with tachyaudio.")
    parser.add_argument("--duration", type=float, default=0.25)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--block-size", type=int, default=256)
    parser.add_argument("--device-id", default=None)
    args = parser.parse_args()

    if args.duration <= 0.0:
        raise SystemExit("--duration must be positive")

    frame_count = int(args.sample_rate * args.duration)

    stream = ta.InputStream(
        sample_rate=args.sample_rate,
        channels=args.channels,
        block_size=args.block_size,
        device_id=args.device_id,
    )
    try:
        stream.start()
        time.sleep(args.duration)
        queued = stream.stats().queued_frames
        data = stream.read(min(frame_count, queued)) if queued else memoryview(b"")
        samples = array("f")
        samples.frombytes(data)
        print(f"captured_frames={len(samples) // args.channels}")
        print(f"rms={rms_level(samples):.6f}")
        print(stream.stats())
    finally:
        stream.close()


if __name__ == "__main__":
    main()
