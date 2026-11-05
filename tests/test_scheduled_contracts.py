"""Scheduler clocks, fault reporting, and cleanup without an audio endpoint."""

import time
import unittest
from array import array
from concurrent.futures import CancelledError
from types import SimpleNamespace
from unittest.mock import Mock, patch

import tachyaudio as ta
from tachyaudio import _scheduled as scheduled


class SchedulerClockTests(unittest.TestCase):
    def test_high_resolution_windows_clock_needs_no_translation(self):
        with patch.object(
            scheduled.time,
            "get_clock_info",
            return_value=SimpleNamespace(implementation="QueryPerformanceCounter()"),
        ):
            self.assertEqual(scheduled._wasapi_clock_offset(), (0, 0))

    def test_legacy_clock_is_correlated_at_tick_transition(self):
        with (
            patch.object(
                scheduled.time,
                "get_clock_info",
                return_value=SimpleNamespace(implementation="GetTickCount64()", resolution=0.016),
            ),
            patch.object(scheduled.time, "perf_counter_ns", side_effect=[100, 200]),
            patch.object(scheduled.time, "monotonic_ns", side_effect=[10, 20]),
        ):
            self.assertEqual(scheduled._wasapi_clock_offset(), (130, 16_000_000))

    def test_stalled_legacy_clock_is_reported(self):
        with (
            patch.object(
                scheduled.time,
                "get_clock_info",
                return_value=SimpleNamespace(implementation="GetTickCount64()", resolution=0.016),
            ),
            patch.object(scheduled.time, "perf_counter_ns", side_effect=[0, 51_000_000]),
            patch.object(scheduled.time, "monotonic_ns", return_value=10),
            self.assertRaisesRegex(RuntimeError, "correlate"),
        ):
            scheduled._wasapi_clock_offset()

    def test_immediate_and_deadline_metrics_cannot_be_confused(self):
        timing = ta.ScheduledTiming(10, 20, 11, 12, 30, 1, immediate=True)
        self.assertEqual(timing.submission_to_onset_seconds, 1e-8)
        with self.assertRaises(ValueError):
            _ = timing.predicted_error_seconds
        with self.assertRaises(ValueError):
            _ = ta.ScheduledTiming(10, 20, 11, 12, 30, 1).submission_to_onset_seconds


class SchedulerFailureTests(unittest.TestCase):
    def setUp(self):
        self.platform = patch.object(scheduled, "sys", SimpleNamespace(platform="darwin"))
        self.platform.start()
        self.addCleanup(self.platform.stop)
        self.handle = Mock()
        self.handle.status.return_value = dict(
            state=1, output_delay_max_us=1000, period_frames=48, buffer_frames=0
        )
        self.factory = patch.object(
            ta._native, "ScheduledOutputStream", return_value=self.handle, create=True
        )
        self.factory.start()
        self.addCleanup(self.factory.stop)
        self.stream = ta.ScheduledOutputStream(channels=1)
        self.addCleanup(self.stream.close)

    def test_start_failure_releases_native_stream(self):
        self.handle.start.side_effect = RuntimeError("device disappeared")
        with self.assertRaisesRegex(RuntimeError, "disappeared"), self.stream:
            self.fail("cannot enter failed stream")
        self.assertTrue(self.stream.failed)
        self.handle.close.assert_called_once()

    def test_configuration_rejects_invalid_leads_and_buffers(self):
        for options in (
            {"minimum_lead_time": -1},
            {"minimum_lead_time": float("inf")},
            {"minimum_lead_time": 2**64},
            {"latency": 0},
            {"latency": 2},
        ):
            with self.subTest(options=options), self.assertRaises(ValueError):
                ta.ScheduledOutputStream(**options)
        with (
            patch.object(scheduled, "sys", SimpleNamespace(platform="win32")),
            self.assertRaises(ValueError),
        ):
            ta.ScheduledOutputStream(latency=0.01)

    def test_deadlines_are_validated_and_native_copy_failures_normalized(self):
        data = array("f", [0])
        for target in (0, -1, float("nan"), float("inf"), 2**65):
            with self.subTest(target=target), self.assertRaises(ValueError):
                self.stream.schedule(data, when=target)
        self.handle.schedule.side_effect = ValueError("insufficient advance notice")
        with self.assertRaises(ta.SchedulingError):
            self.stream.schedule(data, when=time.monotonic() + 1)
        self.handle.schedule.side_effect = ValueError("queue full")
        with self.assertRaisesRegex(ValueError, "queue full"):
            self.stream.schedule(data, when=time.monotonic() + 1)

    def test_native_faults_and_missing_ids_are_reported(self):
        self.stream._latest = 1
        for state in (0, 6, 7, 8, 9, 10):
            self.handle.result.return_value = {"state": state}
            with self.subTest(state=state), self.assertRaises(ta.SchedulingError):
                self.stream.wait_rendered()
        self.handle.result.side_effect = LookupError("unknown stimulus")
        with self.assertRaisesRegex(ValueError, "unknown stimulus"):
            self.stream.wait_rendered(stimulus=999)

    def test_wait_completion_timeout_and_close_are_reported(self):
        self.stream._latest = 1
        self.handle.result.return_value = dict(
            state=4,
            requested_ns=1,
            predicted_onset_ns=2,
            render_callback_ns=1,
            render_complete_ns=2,
            predicted_end_ns=10**20,
            rendered_frames=1,
        )
        with self.assertRaises(TimeoutError):
            self.stream.wait(timeout=0)

        def now():
            self.stream._closed = True
            return 0

        with (
            patch.object(self.stream, "_scheduler_now_ns", side_effect=now),
            self.assertRaises(CancelledError),
        ):
            self.stream.wait()
        self.stream._closed = False

    def test_windows_scheduler_uses_qpc_and_translates_results(self):
        with (
            patch.object(scheduled, "sys", SimpleNamespace(platform="win32")),
            patch.object(scheduled, "_wasapi_clock_offset", return_value=(100, 200)),
        ):
            stream = ta.ScheduledOutputStream(channels=1)
            try:
                stream._latest = 1
                self.handle.result.return_value = dict(
                    state=4,
                    requested_ns=1000,
                    predicted_onset_ns=1100,
                    render_callback_ns=900,
                    render_complete_ns=1000,
                    predicted_end_ns=1200,
                    rendered_frames=1,
                )
                self.assertEqual(stream.wait_rendered().requested_ns, 900)
                with patch.object(scheduled.time, "perf_counter_ns", return_value=123):
                    self.assertEqual(stream._scheduler_now_ns(), 123)
                with self.assertRaises(ta.BackendUnavailable):
                    stream.schedule_asap(array("f", [0]))
            finally:
                stream.close()

    def test_windows_clock_correlation_failure_closes_allocated_stream(self):
        with (
            patch.object(scheduled, "sys", SimpleNamespace(platform="win32")),
            patch.object(scheduled, "_wasapi_clock_offset", side_effect=RuntimeError("clock")),
            self.assertRaisesRegex(RuntimeError, "clock"),
        ):
            ta.ScheduledOutputStream(channels=1)
        self.handle.close.assert_called_once()
