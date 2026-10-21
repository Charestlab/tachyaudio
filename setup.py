from __future__ import annotations

import sys

from setuptools import Extension, setup

extra_link_args: list[str] = []
extra_compile_args: list[str] = []
libraries: list[str] = []

if sys.platform == "darwin":
    extra_link_args.extend(
        ["-framework", "AudioToolbox", "-framework", "CoreAudio", "-framework", "CoreFoundation"]
    )
elif sys.platform.startswith("linux"):
    extra_compile_args.append("-pthread")
    extra_link_args.extend(["-pthread", "-ldl", "-lm"])
elif sys.platform == "win32":
    libraries.extend(["ole32", "uuid", "avrt"])


extensions = [
        Extension(
            "tachyaudio._native",
            sources=["src/tachyaudio/_native.c"],
            depends=["src/tachyaudio/_scheduled_coreaudio.h", "src/tachyaudio/_scheduled_common.h"],
            extra_compile_args=extra_compile_args,
            extra_link_args=extra_link_args,
            libraries=libraries,
        )
    ]

if sys.platform.startswith("linux"):
    extensions.append(Extension(
        "tachyaudio._scheduled_alsa",
        sources=["src/tachyaudio/_scheduled_alsa.c"],
        depends=["src/tachyaudio/_scheduled_common.h"],
        extra_compile_args=["-pthread"],
        extra_link_args=["-pthread", "-ldl", "-lm"],
    ))

setup(ext_modules=extensions)
