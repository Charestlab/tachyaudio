from __future__ import annotations

import argparse
from array import array
from pathlib import Path
import wave

import tachyaudio as ta


def _pcm_sample_to_float(sample: int, sample_width: int) -> float:
    if sample_width == 1:
        return (sample - 128) / 128.0
    return sample / float(1 << (sample_width * 8 - 1))


def _read_pcm_sample(data: bytes, offset: int, sample_width: int) -> int:
    if sample_width == 1:
        return data[offset]
    return int.from_bytes(data[offset : offset + sample_width], "little", signed=True)


def load_wav(
    path: Path,
    *,
    volume: float,
    max_duration: float | None,
) -> tuple[array, int, int, float]:
    """Load an uncompressed PCM WAV file as interleaved float32 samples."""

    with wave.open(str(path), "rb") as wav:
        if wav.getcomptype() != "NONE":
            raise ValueError("only uncompressed PCM WAV files are supported")

        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        total_frames = wav.getnframes()

        if channels < 1:
            raise ValueError("WAV file must have at least one channel")
        if sample_width not in {1, 2, 3, 4}:
            raise ValueError("only 8-, 16-, 24-, and 32-bit PCM WAV files are supported")
        if sample_rate < 1:
            raise ValueError("WAV file sample rate must be positive")

        frame_count = total_frames
        if max_duration is not None:
            frame_count = min(frame_count, int(sample_rate * max_duration))
        raw = wav.readframes(frame_count)

    sample_count = len(raw) // sample_width
    samples = array("f")
    samples.extend(
        _pcm_sample_to_float(_read_pcm_sample(raw, offset, sample_width), sample_width) * volume
        for offset in range(0, sample_count * sample_width, sample_width)
    )
    duration = sample_count / channels / sample_rate
    return samples, sample_rate, channels, duration


def resample_linear(
    samples: array,
    *,
    source_rate: int,
    target_rate: int,
    channels: int,
) -> array:
    """Resample interleaved float32 samples with simple linear interpolation."""

    if source_rate == target_rate:
        return samples

    source_frames = len(samples) // channels
    target_frames = round(source_frames * target_rate / source_rate)
    output = array("f")
    if source_frames == 0:
        return output

    for frame in range(target_frames):
        source_position = frame * source_rate / target_rate
        left_frame = int(source_position)
        right_frame = min(left_frame + 1, source_frames - 1)
        fraction = source_position - left_frame
        for channel in range(channels):
            left = samples[left_frame * channels + channel]
            right = samples[right_frame * channels + channel]
            output.append(left + (right - left) * fraction)

    return output


def main() -> None:
    parser = argparse.ArgumentParser(description="Play an uncompressed PCM WAV file with tachyaudio.")
    parser.add_argument(
        "path",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("reunited.wav"),
        help="WAV file to play; defaults to examples/reunited.wav",
    )
    parser.add_argument("--volume", type=float, default=0.8)
    parser.add_argument("--duration", type=float, default=None, help="Optional maximum seconds to play")
    parser.add_argument(
        "--sample-rate",
        type=int,
        default=48_000,
        help="Playback sample rate; defaults to 48000 for broad device compatibility",
    )
    parser.add_argument("--block-size", type=int, default=256)
    parser.add_argument("--device-id", default=None)
    args = parser.parse_args()

    if args.volume < 0.0:
        raise SystemExit("--volume must be non-negative")
    if args.duration is not None and args.duration <= 0.0:
        raise SystemExit("--duration must be positive")
    if args.sample_rate <= 0:
        raise SystemExit("--sample-rate must be positive")

    samples, source_sample_rate, channels, source_duration = load_wav(
        args.path,
        volume=args.volume,
        max_duration=args.duration,
    )
    samples = resample_linear(
        samples,
        source_rate=source_sample_rate,
        target_rate=args.sample_rate,
        channels=channels,
    )
    duration = len(samples) / channels / args.sample_rate
    print(
        f"playing path={args.path} source_sample_rate={source_sample_rate} "
        f"playback_sample_rate={args.sample_rate} channels={channels} "
        f"duration={duration:.3f}s source_duration={source_duration:.3f}s"
    )
    stats = ta.play(
        samples,
        sample_rate=args.sample_rate,
        channels=channels,
        block_size=args.block_size,
        device_id=args.device_id,
        timeout=duration + 2.0,
    )
    print(stats)


if __name__ == "__main__":
    main()
