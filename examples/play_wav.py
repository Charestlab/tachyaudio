from __future__ import annotations

import argparse
from array import array
from collections.abc import Iterator
import math
from pathlib import Path
import struct
import sys
import time

import tachyaudio as ta


_WAVE_FORMAT_PCM = 0x0001
_WAVE_FORMAT_IEEE_FLOAT = 0x0003
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE
_PCM_SUBFORMAT = bytes.fromhex("0100000000001000800000aa00389b71")
_IEEE_FLOAT_SUBFORMAT = bytes.fromhex("0300000000001000800000aa00389b71")
_EQ_BANDS = (
    ("Low", 80.0),
    ("LowMid", 250.0),
    ("Mid", 750.0),
    ("HighMid", 2_000.0),
    ("High", 6_000.0),
)


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


def upmix_mono_to_stereo(samples: array) -> array:
    output = array("f")
    for sample in samples:
        output.append(sample)
        output.append(sample)
    return output


class EqualizerMeter:
    """Small terminal five-band level meter for already-buffered samples."""

    def __init__(
        self,
        *,
        sample_rate: int,
        channels: int,
        width: int,
        refresh: float,
    ) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
        self.width = width
        self.refresh = refresh
        self.last_update = 0.0
        self.started = time.monotonic()
        self.drawn_lines = 0
        self.dynamic = sys.stdout.isatty()

    def maybe_draw(self, samples: array, *, frame_cursor: int, total_frames: int) -> None:
        now = time.monotonic()
        if now - self.last_update < self.refresh:
            return
        self.last_update = now
        levels = self._band_levels(samples)
        position = frame_cursor / self.sample_rate
        duration = total_frames / self.sample_rate
        if self.dynamic:
            self._draw_dynamic(levels, position=position, duration=duration)
        else:
            self._draw_log_line(levels, position=position, duration=duration)

    def finish(self) -> None:
        if self.drawn_lines:
            print()

    def _bar(self, level: float) -> str:
        scaled = min(1.0, math.sqrt(max(0.0, level)) * 2.5)
        filled = min(self.width, int(round(scaled * self.width)))
        return "█" * filled + "░" * (self.width - filled)

    def _draw_dynamic(self, levels: list[float], *, position: float, duration: float) -> None:
        lines = [f"EQ {position:6.1f}/{duration:6.1f}s"]
        for (name, _frequency), level in zip(_EQ_BANDS, levels, strict=True):
            lines.append(f"{name:7} {self._bar(level)}")

        if self.drawn_lines:
            sys.stdout.write(f"\033[{self.drawn_lines}F")
        for line in lines:
            sys.stdout.write(f"\033[2K{line}\n")
        sys.stdout.flush()
        self.drawn_lines = len(lines)

    def _draw_log_line(self, levels: list[float], *, position: float, duration: float) -> None:
        bars = " ".join(
            f"{name}:{self._bar(level)}"
            for (name, _frequency), level in zip(_EQ_BANDS, levels, strict=True)
        )
        print(f"\r{position:6.1f}/{duration:6.1f}s {bars}", end="", flush=True)
        self.drawn_lines = 1

    def _band_levels(self, samples: array) -> list[float]:
        frame_count = len(samples) // self.channels
        if frame_count == 0:
            return [0.0 for _band in _EQ_BANDS]

        levels: list[float] = []
        for _name, frequency in _EQ_BANDS:
            real = 0.0
            imag = 0.0
            for frame in range(frame_count):
                mono = 0.0
                frame_offset = frame * self.channels
                for channel in range(self.channels):
                    mono += samples[frame_offset + channel]
                mono /= self.channels
                phase = 2.0 * math.pi * frequency * frame / self.sample_rate
                real += mono * math.cos(phase)
                imag -= mono * math.sin(phase)
            levels.append(math.sqrt(real * real + imag * imag) / frame_count)
        return levels


def stream_wav(
    samples: array,
    *,
    sample_rate: int,
    channels: int,
    block_size: int,
    device_id: str | None,
    prebuffer: float,
    chunk_frames: int,
    meter: EqualizerMeter | None,
) -> ta.StreamStats:
    """Play samples through a prebuffered stream to avoid underruns."""

    total_frames = len(samples) // channels
    target_queue_frames = max(block_size, int(sample_rate * prebuffer))
    prebuffer_frames = min(total_frames, target_queue_frames)
    frame_cursor = 0

    stream = ta.OutputStream(
        sample_rate=sample_rate,
        channels=channels,
        block_size=block_size,
        device_id=device_id,
    )
    try:
        if prebuffer_frames:
            stream.write_all(
                samples[: prebuffer_frames * channels],
                timeout=prebuffer + 2.0,
            )
            frame_cursor = prebuffer_frames

        stream.start()
        while frame_cursor < total_frames:
            queued_frames = stream.stats().queued_frames
            if queued_frames >= target_queue_frames:
                sleep_frames = queued_frames - target_queue_frames + block_size
                time.sleep(min(0.05, sleep_frames / sample_rate))
                continue

            writable_frames = min(chunk_frames, target_queue_frames - queued_frames)
            end_frame = min(frame_cursor + writable_frames, total_frames)
            stream.write_all(
                samples[frame_cursor * channels : end_frame * channels],
                timeout=2.0,
            )
            if meter is not None:
                meter.maybe_draw(
                    samples[frame_cursor * channels : end_frame * channels],
                    frame_cursor=end_frame,
                    total_frames=total_frames,
                )
            frame_cursor = end_frame

        if not stream.drain(max(2.0, prebuffer + 2.0)):
            raise TimeoutError("audio playback did not drain before timeout")
        if meter is not None:
            meter.finish()
        return stream.stats()
    finally:
        stream.close()


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
    parser.add_argument("--block-size", type=int, default=1024)
    parser.add_argument("--chunk-frames", type=int, default=4096)
    parser.add_argument("--prebuffer", type=float, default=0.25)
    parser.add_argument("--eq", action="store_true", help="Show a fun five-band terminal meter")
    parser.add_argument("--eq-width", type=int, default=8)
    parser.add_argument("--eq-refresh", type=float, default=0.05)
    parser.add_argument(
        "--mono",
        action="store_true",
        help="Keep mono files mono instead of duplicating them to stereo output",
    )
    parser.add_argument("--device-id", default=None)
    args = parser.parse_args()

    if args.volume < 0.0:
        raise SystemExit("--volume must be non-negative")
    if args.duration is not None and args.duration <= 0.0:
        raise SystemExit("--duration must be positive")
    if args.block_size < 1:
        raise SystemExit("--block-size must be positive")
    if args.chunk_frames < 1:
        raise SystemExit("--chunk-frames must be positive")
    if args.prebuffer < 0.0:
        raise SystemExit("--prebuffer must be non-negative")
    if args.eq_width < 1:
        raise SystemExit("--eq-width must be positive")
    if args.eq_refresh <= 0.0:
        raise SystemExit("--eq-refresh must be positive")

    samples, sample_rate, channels, duration = load_wav(
        args.path,
        volume=args.volume,
        max_duration=args.duration,
    )
    output_channels = channels
    if channels == 1 and not args.mono:
        samples = upmix_mono_to_stereo(samples)
        output_channels = 2
    print(
        f"playing path={args.path} sample_rate={sample_rate} "
        f"source_channels={channels} output_channels={output_channels} "
        f"duration={duration:.3f}s prebuffer={args.prebuffer:.3f}s"
    )
    started = time.monotonic()
    meter = (
        EqualizerMeter(
            sample_rate=sample_rate,
            channels=output_channels,
            width=args.eq_width,
            refresh=args.eq_refresh,
        )
        if args.eq
        else None
    )
    stats = stream_wav(
        samples,
        sample_rate=sample_rate,
        channels=output_channels,
        block_size=args.block_size,
        device_id=args.device_id,
        prebuffer=args.prebuffer,
        chunk_frames=args.chunk_frames,
        meter=meter,
    )
    elapsed = time.monotonic() - started
    print(f"elapsed={elapsed:.3f}s")
    print(stats)


if __name__ == "__main__":
    main()
