"""Shim for the installed ``tachyaudio-audio-report`` command.

The tool now lives in the package (``tachyaudio.audio_report``) and is exposed as
the ``tachyaudio-audio-report`` console script (``pip install tachyaudio[report]``).
This file just forwards to it.
"""
from tachyaudio.audio_report import main

if __name__ == "__main__":
    main()
