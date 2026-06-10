from __future__ import annotations

import sys

from setuptools import Extension, setup


extra_link_args: list[str] = []

if sys.platform == "darwin":
    extra_link_args.extend(
        ["-framework", "AudioToolbox", "-framework", "CoreAudio", "-framework", "CoreFoundation"]
    )


setup(
    ext_modules=[
        Extension(
            "tachyaudio._native",
            sources=["src/tachyaudio/_native.c"],
            extra_link_args=extra_link_args,
        )
    ]
)
