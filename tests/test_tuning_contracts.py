"""Bounded silent probes, failure retention and worker cleanup with fake devices."""

import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, Mock, patch

import tachyaudio as ta
from tachyaudio import _lead_probe as lead
from tachyaudio import tuning


def stream():
    handle = MagicMock()
    handle.__enter__.return_value = handle
    handle.lead_time = 0.01
    handle.minimum_lead_time = 0.012
    handle.failed = False
    handle.status.return_value = {"presentation_latency_us": 100, "minimum_buffer_frames": 0}
    handle.schedule.return_value = 1
    handle.schedule_asap.return_value = 1
    handle.wait.return_value = SimpleNamespace(
        predicted_error_seconds=0.00001,
        predicted_onset_ns=1_020_000_000,
        render_complete_ns=1_001_000_000,
    )
    return handle


class SilentProbeTests(unittest.TestCase):
    def test_deadline_probe_records_success_errors_and_cleans_workers(self):
        for failure in (None, "schedule", "wait", "open", "workers", "fault"):
            handle = stream()
            if failure == "schedule":
                handle.schedule.side_effect = RuntimeError("queue full")
            if failure == "wait":
                handle.wait.side_effect = RuntimeError("missed")
            if failure == "open":
                handle.__enter__.side_effect = RuntimeError("device unavailable")
            handle.failed = failure == "fault"
            event, worker = Mock(), Mock(pid=123)
            event.wait.return_value = failure != "workers"
            worker.is_alive.return_value = True
            with (
                self.subTest(failure=failure),
                patch.object(ta, "ScheduledOutputStream", return_value=handle),
                patch.object(lead.mp, "Event", return_value=event),
                patch.object(lead.mp, "Process", return_value=worker),
                patch.object(lead.time, "monotonic", return_value=1),
                patch.object(lead.time, "sleep"),
            ):
                row = lead._probe("device", 48000, 0.001, None, 2, 1)
            self.assertEqual(row["passed"], failure is None)
            worker.start.assert_called_once()
            worker.terminate.assert_called_once()
            self.assertEqual(worker.join.call_count, 2)
            event.set.assert_called_once()
            if failure in ("schedule", "wait"):
                self.assertEqual(row["failures"], 2)

    def test_immediate_probe_reports_delay_and_delivery_margin(self):
        handle = stream()
        with (
            patch.object(ta, "ScheduledOutputStream", return_value=handle),
            patch.object(lead, "sys", SimpleNamespace(platform="darwin")),
            patch.object(lead.mp, "Event", return_value=Mock()),
            patch.object(lead.time, "monotonic", return_value=1),
            patch.object(lead.time, "sleep"),
        ):
            row = lead._probe(None, 48000, 0.001, None, 2, 0, immediate=True)
        self.assertTrue(row["passed"])
        self.assertEqual(row["immediate_strategy"], "callback")
        self.assertAlmostEqual(row["onset_p99_ms"], 20)
        self.assertAlmostEqual(row["minimum_delivery_margin_ms"], 18.9)
        handle.schedule_asap.assert_called()
        self.assertEqual(handle.minimum_lead_time, 0.012)
        with (
            patch.object(lead, "sys", SimpleNamespace(platform="linux")),
            patch.object(lead.time, "monotonic", return_value=1),
        ):
            lead._submit_immediate(handle, b"silent")
        handle.schedule.assert_called_with(b"silent", when=1.015)

    def test_burn_worker_announces_readiness_and_obeys_stop(self):
        stop, ready = Mock(), Mock()
        stop.is_set.side_effect = [False, True]
        lead._burn(stop, ready)
        ready.set.assert_called_once()

    def test_probe_releases_workers_after_schedule_and_device_errors(self):
        for failure in (None, "schedule", "wait", "fault", "open"):
            handle = stream()
            if failure in ("schedule", "wait"):
                getattr(handle, failure).side_effect = RuntimeError(failure)
            if failure == "open":
                handle.__enter__.side_effect = RuntimeError("open failed")
            handle.failed = failure == "fault"
            context = Mock()
            worker = context.Process.return_value
            with (
                self.subTest(failure=failure),
                patch.object(ta, "ScheduledOutputStream", return_value=handle),
                patch("multiprocessing.get_context", return_value=context),
            ):
                if failure == "open":
                    with self.assertRaisesRegex(RuntimeError, "open failed"):
                        tuning._probe(None, 48000, 1, 0.001, 1, 1, 2)
                else:
                    ok, measured_lead = tuning._probe(None, 48000, 1, 0.001, 1, 1, 2)
                    self.assertEqual(ok, failure is None)
                    self.assertEqual(measured_lead, 0.01)
            worker.start.assert_called_once()
            worker.terminate.assert_called_once()
            worker.join.assert_called_once_with(1)

    def test_fast_settings_search_skips_errors_and_failed_buffers(self):
        with (
            patch.object(tuning, "sys", SimpleNamespace(platform="darwin")),
            patch.object(
                tuning,
                "_probe",
                side_effect=[RuntimeError("unsupported"), (False, 0.02), (True, 0.03)],
            ) as probe,
        ):
            settings = tuning.fastest_safe_settings("output", under_load=False)
        self.assertEqual(settings, tuning.FastSettings(0.001, 0.03))
        self.assertEqual(probe.call_args.args[4], 0)
        with (
            patch.object(tuning, "sys", SimpleNamespace(platform="linux")),
            patch.object(tuning, "_probe", return_value=(False, 0.02)),
            self.assertRaisesRegex(RuntimeError, "no scheduled-output buffer"),
        ):
            tuning.fastest_safe_settings("hw:0,0")
        with (
            patch.object(tuning, "_probe", side_effect=OSError("no device")),
            self.assertRaisesRegex(RuntimeError, "no device"),
        ):
            tuning.fastest_safe_settings()

    def test_worker_start_failure_releases_already_started_workers(self):
        context, first, second = Mock(), Mock(), Mock()
        context.Process.side_effect = [first, second]
        second.start.side_effect = RuntimeError("worker startup failed")
        with (
            patch("multiprocessing.get_context", return_value=context),
            self.assertRaisesRegex(RuntimeError, "worker startup failed"),
        ):
            tuning._probe(None, 48000, 1, 0.001, 2, 1, 1)
        first.terminate.assert_called_once()
        first.join.assert_called_once_with(1)
        second.join.assert_not_called()
