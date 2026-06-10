"""Package-specific exceptions."""


class TachyAudioError(Exception):
    """Base class for all tachyaudio errors."""


class BackendUnavailable(TachyAudioError):
    """Raised when no usable audio backend is available."""


class StreamClosed(TachyAudioError):
    """Raised when stream operations are attempted on a closed stream."""

