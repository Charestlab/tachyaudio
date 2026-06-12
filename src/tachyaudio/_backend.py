"""Backend selection and backend protocol definitions."""

from __future__ import annotations

from typing import Any, Protocol

from tachyaudio._device import DeviceInfo
from tachyaudio._errors import BackendUnavailable


class OutputStreamHandle(Protocol):
    """Backend-owned playback stream handle."""

    def start(self) -> None: ...

    def stop(self) -> None: ...

    def drain(self, timeout: float | None = None) -> bool: ...

    def flush(self) -> None: ...

    def close(self) -> None: ...

    def write(self, frames: Any) -> int: ...

    def stats(self) -> object: ...


class InputStreamHandle(Protocol):
    """Backend-owned capture stream handle."""

    def start(self) -> None: ...

    def stop(self) -> None: ...

    def close(self) -> None: ...

    def flush(self) -> None: ...

    def read(self, frame_count: int) -> memoryview: ...

    def stats(self) -> object: ...


class DuplexStreamHandle(Protocol):
    """Backend-owned full-duplex stream handle."""

    def start(self) -> None: ...

    def stop(self) -> None: ...

    def flush(self) -> None: ...

    def close(self) -> None: ...

    def write(self, frames: Any) -> int: ...

    def read(self, frame_count: int) -> memoryview: ...

    def stats(self) -> object: ...


class AudioBackend(Protocol):
    """Protocol implemented by concrete audio backends."""

    name: str

    def list_devices(self) -> tuple[DeviceInfo, ...]: ...

    def open_output_stream(self, config: object) -> OutputStreamHandle: ...

    def open_input_stream(self, config: object) -> InputStreamHandle: ...

    def open_duplex_stream(self, config: object) -> DuplexStreamHandle: ...


class _UnavailableBackend:
    name = "unavailable"

    def list_devices(self) -> tuple[DeviceInfo, ...]:
        return ()

    def open_output_stream(self, config: object) -> OutputStreamHandle:
        raise BackendUnavailable(
            "no tachyaudio backend is available yet; install or build the native backend"
        )

    def open_input_stream(self, config: object) -> InputStreamHandle:
        raise BackendUnavailable(
            "no tachyaudio backend is available yet; install or build the native backend"
        )

    def open_duplex_stream(self, config: object) -> DuplexStreamHandle:
        raise BackendUnavailable(
            "no tachyaudio backend is available yet; install or build the native backend"
        )


def _load_default_backend() -> AudioBackend:
    try:
        from tachyaudio._native_backend import NativeBackend
    except ImportError:
        return _UnavailableBackend()
    return NativeBackend()


_backend: AudioBackend = _load_default_backend()


def get_backend() -> AudioBackend:
    """Return the active audio backend."""

    return _backend


def set_backend(backend: AudioBackend | None) -> None:
    """Set the active backend.

    Passing `None` restores the default backend. This function is public
    primarily to support controlled integration tests and tachypy adapter
    experiments while the native backend is under development.
    """

    global _backend
    _backend = _load_default_backend() if backend is None else backend


def list_devices() -> tuple[DeviceInfo, ...]:
    """List devices exposed by the active backend."""

    return get_backend().list_devices()
