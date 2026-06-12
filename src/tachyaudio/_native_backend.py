"""Native backend adapter."""

from __future__ import annotations

from tachyaudio import _native
from tachyaudio._device import DeviceInfo, DeviceKind


class NativeOutputStream:
    """Python-facing wrapper around the native output stream handle."""

    def __init__(self, config: object) -> None:
        self._handle = _native.OutputStream(
            config.sample_rate,  # type: ignore[attr-defined]
            config.channels,  # type: ignore[attr-defined]
            config.block_size or 0,  # type: ignore[attr-defined]
            config.device_id,  # type: ignore[attr-defined]
            config.latency or 0.0,  # type: ignore[attr-defined]
        )

    def start(self) -> None:
        self._handle.start()

    def stop(self) -> None:
        self._handle.stop()

    def drain(self, timeout: float | None = None) -> bool:
        return self._handle.drain(-1.0 if timeout is None else timeout)

    def flush(self) -> None:
        self._handle.flush()

    def close(self) -> None:
        self._handle.close()

    def write(self, frames: object) -> int:
        return self._handle.write(frames)

    def stats(self) -> object:
        from tachyaudio._stream import StreamStats

        item = self._handle.stats()
        return StreamStats(
            frames_processed=item["frames_processed"],
            underruns=item["underruns"],
            overruns=item["overruns"],
            estimated_latency=item["estimated_latency"],
            hardware_latency=item["hardware_latency"],
            queued_frames=item["queued_frames"],
            queued_latency=item["queued_latency"],
            buffer_size=item["buffer_size"],
        )


class NativeInputStream:
    """Python-facing wrapper around the native input stream handle."""

    def __init__(self, config: object) -> None:
        self._handle = _native.InputStream(
            config.sample_rate,  # type: ignore[attr-defined]
            config.channels,  # type: ignore[attr-defined]
            config.block_size or 0,  # type: ignore[attr-defined]
            config.device_id,  # type: ignore[attr-defined]
            config.latency or 0.0,  # type: ignore[attr-defined]
        )

    def start(self) -> None:
        self._handle.start()

    def stop(self) -> None:
        self._handle.stop()

    def close(self) -> None:
        self._handle.close()

    def flush(self) -> None:
        self._handle.flush()

    def read(self, frame_count: int) -> memoryview:
        return memoryview(self._handle.read(frame_count))

    def stats(self) -> object:
        from tachyaudio._stream import StreamStats

        item = self._handle.stats()
        return StreamStats(
            frames_processed=item["frames_processed"],
            underruns=item["underruns"],
            overruns=item["overruns"],
            estimated_latency=item["estimated_latency"],
            hardware_latency=item["hardware_latency"],
            queued_frames=item["queued_frames"],
            queued_latency=item["queued_latency"],
            buffer_size=item["buffer_size"],
        )


class NativeBackend:
    """Adapter from the native extension to the backend protocol."""

    name = _native.backend_name()

    def list_devices(self) -> tuple[DeviceInfo, ...]:
        devices: list[DeviceInfo] = []
        for item in _native.list_devices():
            sample_rate = item["default_sample_rate"]
            devices.append(
                DeviceInfo(
                    id=item["id"],
                    name=item["name"],
                    kind=DeviceKind(item["kind"]),
                    channels=item["channels"],
                    default_sample_rate=round(sample_rate) if sample_rate else None,
                    is_default=item["is_default"],
                )
            )
        return tuple(devices)

    def open_output_stream(self, config: object) -> NativeOutputStream:
        return NativeOutputStream(config)

    def open_input_stream(self, config: object) -> NativeInputStream:
        return NativeInputStream(config)
