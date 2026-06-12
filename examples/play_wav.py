from __future__ import annotations

import argparse
from array import array
from collections.abc import Iterator
from pathlib import Path
import struct
import sys

import tachyaudio as ta


_WAVE_FORMAT_PCM = 0x0001
_WAVE_FORMAT_IEEE_FLOAT = 0x0003
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE
_PCM_SUBFORMAT = bytes.fromhex("0100000000001000800000aa00389b71")
_IEEE_FLOAT_SUBFORMAT = bytes.fromhex("0300000000001000800000aa00389b71")


def _pcm_sample_to_float(sample: int, sample_width: int) -> float:
    if sample_width == 1:
        return (sample - 128) / 128.0
    return sample / float(1 << (sample_width * 8 - 1))


def _read_pcm_sample(data: bytes, offset: int, sample_width: int) -> int:
    if sample_width == 1:
        return data[offset]
    return int.from_bytes(data[offset : offset + sample_width], "little", signed=True)


def _iter_chunks(data: bytes) -> Iterator[tuple[bytes, bytes]]:
    offset = 12
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = int.from_bytes(data[offset + 4 : offset + 8], "little")
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            raise ValueError("WAV chunk extends beyond end of file")
        yield chunk_id, data[chunk_start:chunk_end]
        offset = chunk_end + (chunk_size % 2)


def _parse_wav(path: Path) -> tuple[int, int, int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("file is not a RIFF/WAVE file")

    format_tag: int | None = None
    channels: int | None = None
    sample_rate: int | None = None
    bits_per_sample: int | None = None
    audio_data: bytes | None = None

    for chunk_id, chunk in _iter_chunks(data):
        if chunk_id == b"fmt ":
            if len(chunk) < 16:
                raise ValueError("WAV fmt chunk is too short")
            format_tag, channels, sample_rate, _, _, bits_per_sample = struct.unpack(
                "<HHIIHH",
                chunk[:16],
            )
            if format_tag == _WAVE_FORMAT_EXTENSIBLE:
                if len(chunk) < 40:
                    raise ValueError("WAV extensible fmt chunk is too short")
                subformat = chunk[24:40]
                if subformat == _PCM_SUBFORMAT:
                    format_tag = _WAVE_FORMAT_PCM
                elif subformat == _IEEE_FLOAT_SUBFORMAT:
                    format_tag = _WAVE_FORMAT_IEEE_FLOAT
                else:
                    raise ValueError("unsupported WAV extensible subformat")
        elif chunk_id == b"data":
            audio_data = chunk

    if format_tag is None or channels is None or sample_rate is None or bits_per_sample is None:
        raise ValueError("WAV file is missing a fmt chunk")
    if audio_data is None:
        raise ValueError("WAV file is missing a data chunk")

    sample_width = bits_per_sample // 8
    if bits_per_sample % 8 != 0 or sample_width < 1:
        raise ValueError("WAV bit depth must be byte-aligned")
    if channels < 1:
        raise ValueError("WAV file must have at least one channel")
    if sample_rate < 1:
        raise ValueError("WAV file sample rate must be positive")

    return format_tag, channels, sample_rate, sample_width, audio_data


def load_wav(
    path: Path,
    *,
    volume: float,
    max_duration: float | None,
) -> tuple[array, int, int, float]:
    """Load an uncompressed WAV file as interleaved float32 samples."""

    format_tag, channels, sample_rate, sample_width, raw = _parse_wav(path)
    if max_duration is not None:
        max_bytes = int(sample_rate * max_duration) * channels * sample_width
        raw = raw[:max_bytes]

    if format_tag == _WAVE_FORMAT_IEEE_FLOAT:
        if sample_width != 4:
            raise ValueError("only 32-bit float WAV files are supported")
        samples = array("f")
        samples.frombytes(raw[: len(raw) - (len(raw) % sample_width)])
        if sys.byteorder != "little":
            samples.byteswap()
        if volume != 1.0:
            samples = array("f", (sample * volume for sample in samples))
    elif format_tag == _WAVE_FORMAT_PCM:
        if sample_width not in {1, 2, 3, 4}:
            raise ValueError("only 8-, 16-, 24-, and 32-bit PCM WAV files are supported")
        sample_count = len(raw) // sample_width
        samples = array("f")
        samples.extend(
            _pcm_sample_to_float(_read_pcm_sample(raw, offset, sample_width), sample_width) * volume
            for offset in range(0, sample_count * sample_width, sample_width)
        )
    else:
        raise ValueError("only PCM and 32-bit float WAV files are supported")

    sample_count = len(samples)
    duration = sample_count / channels / sample_rate
    return samples, sample_rate, channels, duration


def main() -> None:
    parser = argparse.ArgumentParser(description="Play an uncompressed WAV file with tachyaudio.")
    parser.add_argument(
        "path",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("reunited.wav"),
        help="WAV file to play; defaults to examples/reunited.wav",
    )
    parser.add_argument("--volume", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=None, help="Optional maximum seconds to play")
    parser.add_argument("--block-size", type=int, default=256)
    parser.add_argument("--device-id", default=None)
    args = parser.parse_args()

    if args.volume < 0.0:
        raise SystemExit("--volume must be non-negative")
    if args.duration is not None and args.duration <= 0.0:
        raise SystemExit("--duration must be positive")

    samples, sample_rate, channels, duration = load_wav(
        args.path,
        volume=args.volume,
        max_duration=args.duration,
    )
    print(
        f"playing path={args.path} sample_rate={sample_rate} "
        f"channels={channels} duration={duration:.3f}s"
    )
    stats = ta.play(
        samples,
        sample_rate=sample_rate,
        channels=channels,
        block_size=args.block_size,
        device_id=args.device_id,
        timeout=duration + 2.0,
    )
    print(stats)


if __name__ == "__main__":
    main()
