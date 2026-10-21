from __future__ import annotations

import unittest
from array import array

import tachyaudio as ta


class BoundedOutput:
    def __init__(self) -> None:
        self.started = False
        self.closed = False
        self.accepted = bytearray()
        self.start_error = False
        self.first_write_started: bool | None = None

    def write(self, frames: memoryview) -> int:
        if self.first_write_started is None:
            self.first_write_started = self.started
        if not self.started and self.accepted:
            return 0
        portion = memoryview(frames).cast("B")[:12]
        self.accepted.extend(portion)
        return len(portion) // 4

    def start(self) -> None:
        if self.start_error:
            raise RuntimeError("device start failed")
        self.started = True

    def drain(self, timeout: float | None = None) -> bool:
        return self.started

    def close(self) -> None:
        self.closed = True

    def stats(self) -> ta.StreamStats:
        return ta.StreamStats(frames_processed=len(self.accepted) // 4)


class PrefillTests(unittest.TestCase):
    def setUp(self) -> None:
        self.handle = BoundedOutput()
        handle = self.handle

        class Backend:
            name = "bounded-test"

            def open_output_stream(self, config: object) -> BoundedOutput:
                return handle

        ta.set_backend(Backend())

    def tearDown(self) -> None:
        ta.set_backend(None)

    def test_clip_larger_than_ring_prefills_then_starts_and_preserves_samples(self) -> None:
        frames = array("f", range(20))
        stats = ta.play(frames, channels=1, timeout=1)
        self.assertFalse(self.handle.first_write_started)
        self.assertEqual(self.handle.accepted, frames.tobytes())
        self.assertEqual(stats.frames_processed, 20)
        self.assertTrue(self.handle.closed)

    def test_start_failure_closes_prefilled_stream(self) -> None:
        self.handle.start_error = True
        with self.assertRaisesRegex(RuntimeError, "device start failed"):
            ta.play(array("f", [0.0]), channels=1)
        self.assertTrue(self.handle.closed)

    def test_invalid_timeout_closes_without_starting(self) -> None:
        with self.assertRaises(ValueError):
            ta.play(array("f", [0.0]), channels=1, timeout=-1)
        self.assertFalse(self.handle.started)
        self.assertTrue(self.handle.closed)
