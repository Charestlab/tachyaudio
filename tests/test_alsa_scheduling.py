"""Exercise the real C worker against a simulated ALSA ABI, not physical hardware."""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import time
import unittest
from array import array
from pathlib import Path
from unittest.mock import patch


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux extension routing")
class LinuxRoutingTests(unittest.TestCase):
    def test_public_api_requires_explicit_hardware_device(self):
        import tachyaudio as ta

        with self.assertRaisesRegex(ValueError, "device_id"):
            ta.ScheduledOutputStream()


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which("cc"),
                     "requires a POSIX C compiler")
class AlsaWorkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(__file__).resolve().parents[1]
        library = Path(cls.temp.name) / "libfakealsa.so"
        module_path = Path(cls.temp.name) / "_scheduled_alsa.so"
        common = ["cc", "-shared", "-fPIC", "-pthread"]
        subprocess.run(common + [str(root / "tests/fake_alsa.c"), "-o", str(library)],
                       check=True, capture_output=True)
        linker = ["-undefined", "dynamic_lookup"] if sys.platform == "darwin" else ["-ldl", "-lm"]
        subprocess.run(common + ["-I" + sysconfig.get_path("include"),
                                f'-DTACHY_ALSA_LIBRARY="{library}"',
                                str(root / "src/tachyaudio/_scheduled_alsa.c"),
                                "-o", str(module_path)] + linker,
                       check=True, capture_output=True)
        spec = importlib.util.spec_from_file_location("_scheduled_alsa", module_path)
        cls.native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.native)

    def open(self):
        stream = self.native.ScheduledOutputStream(48000, 2, "hw:FAKE,0")
        self.addCleanup(stream.close)
        stream.start()
        return stream

    def wait_state(self, stream, states):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            status = stream.status()
            if status["state"] in states:
                return status
            time.sleep(0.001)
        self.fail(f"timeout: {stream.status()}")

    def test_schedule_and_reuse_without_python_at_onset(self):
        stream = self.open()
        for _unused in range(2):
            target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 150_000_000
            stream.schedule(array("f", [0.25, -0.25] * 1000), target, 50_000_000)
            previous = sys.getswitchinterval()
            try:
                sys.setswitchinterval(0.5)
                while time.clock_gettime_ns(time.CLOCK_MONOTONIC) < target + 30_000_000:
                    pass
            finally:
                sys.setswitchinterval(previous)
            status = self.wait_state(stream, {4, 6, 7, 8, 9})
            self.assertEqual(status["state"], 4)
            self.assertEqual(status["rendered_frames"], 1000)
            error = status["predicted_onset_ns"] - target
            self.assertGreaterEqual(error, 0)
            self.assertLessEqual(error, 20834)

    def test_cancel_overlap_and_close(self):
        stream = self.open()
        frames = array("f", [0, 0])
        target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 500_000_000
        stream.schedule(frames, target)
        with self.assertRaises(ValueError):
            stream.schedule(frames, target)
        stream.cancel()
        self.assertEqual(stream.status()["state"], 5)
        stream.close()
        stream.close()
        with self.assertRaises(RuntimeError):
            stream.start()

    def test_partial_write_fails_without_recovery(self):
        with patch.dict(os.environ, {"TACHY_FAKE_PARTIAL": "1"}):
            stream = self.open()
            self.assertEqual(self.wait_state(stream, {8})["state"], 8)
            with self.assertRaises(RuntimeError):
                stream.start()

    def test_invalid_clock_fails_explicitly(self):
        with patch.dict(os.environ, {"TACHY_FAKE_CLOCK": "1"}):
            stream = self.open()
            self.assertEqual(self.wait_state(stream, {7})["state"], 7)

    def test_integer_format_negotiation(self):
        with patch.dict(os.environ, {"TACHY_FAKE_S16": "1"}):
            stream = self.open()
            self.assertEqual(stream.status()["device_format"], "s16_le")

    def test_s32_format_negotiation(self):
        with patch.dict(os.environ, {"TACHY_FAKE_S32": "1"}):
            stream = self.open()
            self.assertEqual(stream.status()["device_format"], "s32_le")

    def test_rt_priority_flag_is_present(self):
        stream = self.open()
        self.assertIn(stream.status()["rt_priority"], (0, 1))

    def test_explicit_device_required(self):
        for device in (None, "default", "pulse", "plughw:0,0"):
            with self.subTest(device=device), self.assertRaises(ValueError):
                self.native.ScheduledOutputStream(48000, 2, device)

    def test_nonfinite_samples_rejected(self):
        stream = self.open()
        with self.assertRaises(ValueError):
            stream.schedule(array("f", [float("nan"), 0]), time.monotonic_ns() + 1_000_000_000)
