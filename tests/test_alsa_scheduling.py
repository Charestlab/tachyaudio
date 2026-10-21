"""Exercise the real C worker against a simulated ALSA ABI, not physical hardware."""

from __future__ import annotations

import ctypes
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
        cls.fake = ctypes.CDLL(str(library))
        cls.fake.tachy_fake_onset_ns.restype = ctypes.c_longlong
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

    def wait_result(self, stream, stimulus):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            record = stream.result(stimulus)
            if record["state"] not in (2, 3):
                return record
            time.sleep(0.001)
        self.fail(f"timeout: {stream.result(stimulus)}")

    def test_queued_stimuli_render_in_order_at_their_onsets(self):
        stream = self.open()
        now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        targets = [now + 150_000_000 + step * 30_000_000 for step in range(3)]
        ids = [stream.schedule(array("f", [0.25, 0.25] * 480), target, 50_000_000)
               for target in targets]
        self.assertEqual(len(set(ids)), 3)
        self.assertEqual(stream.status()["queued"], 3)
        for stimulus, target in zip(ids, targets, strict=True):
            record = self.wait_result(stream, stimulus)
            self.assertEqual(record["state"], 4)
            self.assertEqual(record["rendered_frames"], 480)
            self.assertGreaterEqual(record["predicted_onset_ns"] - target, 0)
            self.assertLessEqual(record["predicted_onset_ns"] - target, 20834)
        self.assertEqual(stream.status()["queued"], 0)

    def test_back_to_back_accepted_overlap_rejected(self):
        stream = self.open()
        target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 150_000_000
        first = stream.schedule(array("f", [0.1, 0.1] * 1000), target, 50_000_000)
        with self.assertRaisesRegex(ValueError, "overlaps"):
            stream.schedule(array("f", [0.1, 0.1] * 10), target + 10_000_000, 50_000_000)
        second = stream.schedule(array("f", [0.1, 0.1] * 1000),
                                 target + round(1000 * 1e9 / 48000), 50_000_000)
        for stimulus in (first, second):
            self.assertEqual(self.wait_result(stream, stimulus)["state"], 4)
        gap = (stream.result(second)["predicted_onset_ns"]
               - stream.result(first)["predicted_end_ns"])
        self.assertLessEqual(abs(gap), 20834)

    def test_cancel_one_missed_one_others_continue(self):
        stream = self.open()
        now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        missed = stream.schedule(array("f", [0.1, 0.1] * 10), now + 1_000_000, 0)
        cancelled = stream.schedule(array("f", [0.1, 0.1] * 10), now + 150_000_000, 0)
        kept = stream.schedule(array("f", [0.1, 0.1] * 10), now + 200_000_000, 0)
        stream.cancel(cancelled)
        self.assertEqual(self.wait_result(stream, missed)["state"], 6)
        self.assertEqual(stream.result(cancelled)["state"], 5)
        self.assertEqual(self.wait_result(stream, kept)["state"], 4)
        with self.assertRaises(LookupError):
            stream.result(10_000)

    def test_queue_holds_32_stimuli(self):
        stream = self.open()
        target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 500_000_000
        for index in range(32):
            stream.schedule(array("f", [0.0, 0.0]), target + index * 1_000_000, 0)
        with self.assertRaisesRegex(ValueError, "too many"):
            stream.schedule(array("f", [0.0, 0.0]), target + 40_000_000, 0)
        stream.cancel()
        self.assertEqual(stream.status()["queued"], 0)

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

    def test_rt_priority_is_reported(self):
        status = self.open().status()
        self.assertIn(status["rt_method"], ("none", "sched_setscheduler", "rtkit"))
        self.assertEqual(status["rt_priority"] > 0, status["rt_method"] != "none")

    def test_usb_style_status_keeps_true_onset_accurate(self):
        # Delay beyond the ring, +/-1 ms delay noise and 4 ms stale timestamps:
        # a single status read is off by milliseconds; the clock fit is not.
        with patch.dict(os.environ, {"TACHY_FAKE_USB": "1"}):
            stream = self.open()
            time.sleep(0.6)
            target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 150_000_000
            stream.schedule(array("f", [0.5, 0.5] * 100), target, 50_000_000)
            status = self.wait_state(stream, {4, 6, 7, 8, 9})
            self.assertEqual(status["state"], 4)
            self.assertEqual(status["clock_resets"], 0)
            self.assertGreater(status["clock_observations"], 100)
            self.assertLess(abs(self.fake.tachy_fake_onset_ns() - target), 250_000)

    def test_mono_is_duplicated_on_stereo_only_device(self):
        with patch.dict(os.environ, {"TACHY_FAKE_STEREO_ONLY": "1"}):
            stream = self.native.ScheduledOutputStream(48000, 1, "hw:FAKE,0")
            self.addCleanup(stream.close)
            stream.start()
            self.assertEqual(stream.status()["device_channels"], 2)
            target = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 150_000_000
            stream.schedule(array("f", [0.5] * 100), target, 50_000_000)
            status = self.wait_state(stream, {4, 6, 7, 8, 9})
            self.assertEqual(status["state"], 4)
            self.assertEqual(status["rendered_frames"], 100)
            self.assertGreaterEqual(self.fake.tachy_fake_onset_ns(), 0)
            with self.assertRaises(RuntimeError):
                self.native.ScheduledOutputStream(48000, 3, "hw:FAKE,0")

    def test_latency_is_configurable_and_validated(self):
        stream = self.native.ScheduledOutputStream(48000, 2, "hw:FAKE,0", 5000)
        self.addCleanup(stream.close)
        self.assertEqual(stream.status()["period_frames"], 480)
        self.assertFalse(stream.status()["buffer_automatic"])
        automatic = self.native.ScheduledOutputStream(48000, 2, "hw:FAKE,0")
        self.addCleanup(automatic.close)
        self.assertTrue(automatic.status()["buffer_automatic"])
        for buffer_us in (999, 1_000_001):
            with self.subTest(buffer_us=buffer_us), self.assertRaises(ValueError):
                self.native.ScheduledOutputStream(48000, 2, "hw:FAKE,0", buffer_us)

    def test_explicit_device_required(self):
        for device in (None, "default", "pulse", "plughw:0,0"):
            with self.subTest(device=device), self.assertRaises(ValueError):
                self.native.ScheduledOutputStream(48000, 2, device)

    def test_nonfinite_samples_rejected(self):
        stream = self.open()
        with self.assertRaises(ValueError):
            stream.schedule(array("f", [float("nan"), 0]), time.monotonic_ns() + 1_000_000_000)
