"""Device metadata exposed by audio backends."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class DeviceKind(str, Enum):
    """Audio device direction."""

    INPUT = "input"
    OUTPUT = "output"
    DUPLEX = "duplex"


@dataclass(frozen=True, slots=True)
class DeviceInfo:
    """Description of an audio device reported by the active backend."""

    id: str
    name: str
    kind: DeviceKind
    channels: int
    default_sample_rate: int | None = None
    is_default: bool = False

    def __post_init__(self) -> None:
        if not self.id:
            raise ValueError("device id cannot be empty")
        if not self.name:
            raise ValueError("device name cannot be empty")
        if self.channels < 1:
            raise ValueError("device channels must be at least 1")
        if self.default_sample_rate is not None and self.default_sample_rate < 1:
            raise ValueError("default sample rate must be positive")

