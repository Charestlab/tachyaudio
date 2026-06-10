from __future__ import annotations

import argparse
import math
from array import array

import tachyaudio as ta


def build_tone(
    *,
    frequency: float,
    duration: float,
    amplitude: float,
    sample_rate: int,
    channels: int,
) -> array:
    frames = int(sample_rate * duration)
    samples = array("f")

    for frame in range(frames):
        value = amplitude * math.sin(2.0 * math.pi * frequency * frame / sample_rate)
        for _ in range(channels):
            samples.append(value)

    return samples


def main() -> None:
    parser = argparse.ArgumentParser(description="Play a simple sine tone with tachyaudio.")
    parser.add_argument("--frequency", type=float, default=440.0)
    parser.add_argument("--duration", type=float, default=0.25)
    parser.add_argument("--amplitude", type=float, default=0.08)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--block-size", type=int, default=256)
    parser.add_argument("--device-id", default=None)
    args = parser.parse_args()

    if args.amplitude < 0.0 or args.amplitude > 1.0:
        raise SystemExit("--amplitude must be between 0.0 and 1.0")
    if args.duration <= 0.0:
        raise SystemExit("--duration must be positive")

    samples = build_tone(
        frequency=args.frequency,
        duration=args.duration,
        amplitude=args.amplitude,
        sample_rate=args.sample_rate,
        channels=args.channels,
    )

    stats = ta.play(
        samples,
        sample_rate=args.sample_rate,
        channels=args.channels,
        block_size=args.block_size,
        device_id=args.device_id,
        timeout=args.duration + 2.0,
    )
    print(stats)


if __name__ == "__main__":
    main()
