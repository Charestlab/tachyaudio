"""Low-level audio primitives for tachypy and Python experiments."""

from tachyaudio._alsa_devices import AlsaOutput, list_alsa_outputs
from tachyaudio._backend import get_backend, list_devices, set_backend
from tachyaudio._device import DeviceInfo, DeviceKind
from tachyaudio._errors import BackendUnavailable, StreamClosed, TachyAudioError
from tachyaudio._scheduled import ScheduledOutputStream, ScheduledTiming, SchedulingError
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
from tachyaudio.tuning import FastSettings, fastest_safe_settings

__all__ = [
    "FastSettings",
    "fastest_safe_settings",
    "AlsaOutput",
    "BackendUnavailable",
    "DeviceInfo",
    "DeviceKind",
    "DuplexStream",
    "DuplexStreamConfig",
    "DuplexStreamStats",
    "InputStream",
    "OutputStream",
    "ScheduledOutputStream",
    "list_alsa_outputs",
    "ScheduledTiming",
    "SchedulingError",
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
