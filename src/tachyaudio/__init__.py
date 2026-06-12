"""Low-level audio primitives for tachypy and Python experiments."""

from tachyaudio._backend import get_backend, list_devices, set_backend
from tachyaudio._device import DeviceInfo, DeviceKind
from tachyaudio._errors import BackendUnavailable, StreamClosed, TachyAudioError
from tachyaudio._stream import (
    DuplexStream,
    DuplexStreamConfig,
    DuplexStreamStats,
    InputStream,
    OutputStream,
    StreamConfig,
    StreamStats,
    play,
)
from tachyaudio._version import __version__

__all__ = [
    "BackendUnavailable",
    "DeviceInfo",
    "DeviceKind",
    "DuplexStream",
    "DuplexStreamConfig",
    "DuplexStreamStats",
    "InputStream",
    "OutputStream",
    "StreamClosed",
    "StreamConfig",
    "StreamStats",
    "TachyAudioError",
    "__version__",
    "get_backend",
    "list_devices",
    "play",
    "set_backend",
]
