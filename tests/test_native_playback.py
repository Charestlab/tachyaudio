from __future__ import annotations

import os
import sys
import time
import unittest
from array import array
from concurrent.futures import CancelledError

import tachyaudio as ta


@unittest.skipUnless(os.environ.get("TACHYAUDIO_TEST_HARDWARE") == "1", "requires audio output")
class NativePlaybackTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "macOS device-clock scheduler")
    def test_scheduled_render_runs_while_python_holds_the_gil(self) -> None:
        with ta.ScheduledOutputStream(channels=1) as stream:
            target = time.monotonic() + 0.12
            stream.schedule(array("f", [0.0]) * 480, when=target)
            previous = sys.getswitchinterval()
            try:
                sys.setswitchinterval(0.5)
                until = time.monotonic() + 0.25
                while time.monotonic() < until:
                    pass
            finally:
                sys.setswitchinterval(previous)
            timing = stream.wait(timeout=1)
            self.assertGreaterEqual(timing.predicted_error_seconds, -1e-7)
            self.assertLessEqual(timing.predicted_error_seconds, 1 / 48000 + 1e-9)
            self.assertLess(timing.render_callback_ns, timing.predicted_onset_ns)
            self.assertEqual(timing.rendered_frames, 480)

    @unittest.skipUnless(sys.platform == "darwin", "macOS device-clock scheduler")
    def test_scheduled_cancel_overlap_and_reuse(self) -> None:
        with ta.ScheduledOutputStream(channels=1) as stream:
            target = time.monotonic() + 1
            stream.schedule(array("f", [0.0]) * 480, when=target)
            # Overlapping stimuli are mixed, not rejected.
            stream.schedule(array("f", [0.0]), when=target + 0.001)
            stream.cancel()
            with self.assertRaises(CancelledError):
                stream.wait(timeout=1)
            stream.schedule(array("f", [0.0]), when=time.monotonic() + 0.12)
            self.assertEqual(stream.wait(timeout=1).rendered_frames, 1)

    @unittest.skipUnless(sys.platform == "darwin", "macOS queue instrumentation")
    def test_native_timestamps_share_python_clock_and_identify_data_buffer(self) -> None:
        stream = ta.OutputStream(sample_rate=48_000, channels=1)
        try:
            native = stream._handle._handle
            before_ns = time.monotonic_ns()
            initial = native._timing()
            after_ns = time.monotonic_ns()
            self.assertLessEqual(before_ns, initial["clock_now_ns"])
            self.assertLessEqual(initial["clock_now_ns"], after_ns)
            stream.start()
            written_ns = time.monotonic_ns()
            stream.write(array("f", [0.0]))
            self.assertTrue(stream.drain(timeout=2))
            timing = native._timing()
            self.assertEqual(timing["data_buffer_count"], 1)
            self.assertLessEqual(written_ns, timing["data_copy_ns"])
            self.assertLessEqual(timing["data_copy_ns"], timing["enqueue_begin_ns"])
            self.assertLessEqual(timing["enqueue_begin_ns"], timing["enqueue_end_ns"])
            self.assertLessEqual(timing["data_copy_ns"], timing["data_complete_ns"])
            time.sleep(0.03)
            self.assertEqual(native._timing()["data_buffer_count"], 1)
        finally:
            stream.close()

    def test_short_finite_clip_does_not_count_tail_silence_as_underruns(self) -> None:
        stats = ta.play(array("f", [0.0]) * 64, channels=1, timeout=2)
        self.assertEqual(stats.underruns, 0)

    def test_drain_keeps_engine_running_and_idle_is_not_an_underrun(self) -> None:
        stream = ta.OutputStream(sample_rate=48_000, channels=1)
        try:
            stream.start()
            self.assertTrue(stream.drain(timeout=2))
            before = stream.stats()
            time.sleep(0.1)
            after = stream.stats()
            self.assertGreater(after.frames_processed, before.frames_processed)
            self.assertEqual(after.underruns, before.underruns)
            for _ in range(3):
                stream.write_all(array("f", [0.0]) * 960, timeout=2)
                self.assertTrue(stream.drain(timeout=2))
        finally:
            stream.close()

    def test_long_clip_streams_beyond_ring_capacity(self) -> None:
        stats = ta.play(array("f", [0.0]) * 60_000, channels=1, timeout=3)
        self.assertGreaterEqual(stats.frames_processed, 60_000)

    def test_prefilled_clip_does_not_drain_before_playback(self) -> None:
        stream = ta.OutputStream(sample_rate=48_000, channels=1)
        try:
            self.assertEqual(stream.write(array("f", [0.0]) * 4800), 4800)
            self.assertFalse(stream.drain(timeout=0))
            stream.start()
            self.assertFalse(stream.drain(timeout=0))
            self.assertTrue(stream.drain(timeout=2))
        finally:
            stream.close()
