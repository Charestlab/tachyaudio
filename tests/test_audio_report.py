"""Diagnostics exercise synthetic audio and fake endpoints, never speakers or a mic."""

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, Mock, patch

import numpy as np

import tachyaudio as ta
from tachyaudio import _lead_probe
from tachyaudio import audio_report as report


def measurement():
    return dict(
        output_delay_ms=10.0,
        presentation_latency_ms=2.0,
        input_hardware_latency_ms=3.0,
        scheduler_onset_error_us_std=1.0,
        scheduler_onset_error_us_max=20.0,
        lead_time_ms=12.0,
        period_frames=48,
        detections_valid=False,
        jitter_std_us=3.0,
        jitter_max_us=4.0,
    )


class SignalTests(unittest.TestCase):
    def test_chirp_and_matched_filter_recover_synthetic_onsets(self):
        signal = report._chirp()
        self.assertEqual(signal.dtype, np.float32)
        self.assertEqual(len(signal), report._CHIRP_FRAMES)
        self.assertLessEqual(float(np.max(np.abs(signal))), report._CHIRP_AMP)
        audio = np.zeros(24000, dtype=np.float32)
        expected = [200, 10000, 20000]
        for index in expected:
            audio[index : index + len(signal)] += signal
        indices, peaks = report._detect(audio, signal, len(expected))
        np.testing.assert_array_equal(indices, expected)
        self.assertTrue(np.all(peaks > 0))

    def test_device_selection_and_names(self):
        devices = [
            SimpleNamespace(id="first", name="First", kind=ta.DeviceKind.OUTPUT, is_default=False),
            SimpleNamespace(
                id="default", name="Default", kind=ta.DeviceKind.OUTPUT, is_default=True
            ),
        ]
        with patch.object(ta, "list_devices", return_value=devices):
            self.assertEqual(report._default_device("output"), "default")
            self.assertEqual(report._device_name("first"), "First")
            self.assertEqual(report._device_name("unknown"), "unknown")
            with self.assertRaises(report.AudioReportError):
                report._default_device("input")
        with patch.object(ta, "list_devices", return_value=devices[:1]):
            self.assertEqual(report._default_device("output"), "first")

    def test_capture_peak_handles_empty_reads_and_device_errors(self):
        capture = MagicMock()
        capture.__enter__.return_value = capture
        for frames, peak in ((np.array([0.2, -0.4], dtype="float32").tobytes(), 0.4), (b"", 0)):
            capture.read.side_effect = [b"", frames]
            with (
                patch.object(ta, "InputStream", return_value=capture),
                patch.object(report.time, "monotonic", side_effect=[0, 0, 0.1, 0.4]),
                patch.object(report.time, "sleep"),
            ):
                self.assertAlmostEqual(report._capture_peak("mic"), peak)
        with (
            patch.object(ta, "InputStream", side_effect=OSError("permission denied")),
            self.assertRaisesRegex(report.AudioReportError, "Could not open"),
        ):
            report._capture_peak("mic")

    def test_silent_microphone_has_platform_specific_guidance(self):
        for platform, text in (
            ("darwin", "System Settings"),
            ("linux", "alsamixer"),
            ("win32", "Privacy"),
            ("other", "permission"),
        ):
            with (
                self.subTest(platform=platform),
                patch.object(report, "sys", SimpleNamespace(platform=platform)),
                patch.object(report, "_capture_peak", return_value=0),
                self.assertRaisesRegex(report.AudioReportError, text),
            ):
                report._check_microphone("mic")
        with patch.object(report, "_capture_peak", return_value=0.01):
            report._check_microphone("mic")

    def test_measurement_separates_scheduler_errors_from_acoustic_residuals(self):
        for condition in ("valid", "silence", "reader_error"):
            capture, output, event = MagicMock(), MagicMock(), Mock()
            capture.__enter__.return_value = capture
            output.__enter__.return_value = output
            samples = (
                np.ones(5000, dtype="float32")
                if condition == "valid"
                else np.zeros(5000, dtype="float32")
            )
            capture.read.side_effect = (
                [b"", samples.tobytes()]
                if condition != "reader_error"
                else RuntimeError("capture failed")
            )
            capture.stats.return_value = ta.StreamStats(hardware_latency=0.002)
            event.is_set.side_effect = [False, False, True]
            output.lead_time = 0.01
            output.status.return_value = dict(
                output_delay_us=10000, presentation_latency_us=2000, period_frames=48
            )
            output.wait.return_value = ta.ScheduledTiming(1000, 2000, 500, 900, 3000, 1920)
            spacing = np.cumsum(np.random.default_rng(731).uniform(0.13, 0.19, 3))
            spacing -= spacing[0]
            indices = np.round((spacing + 0.1) * report.RATE).astype(int)

            def thread(*, target, **kwargs):
                return SimpleNamespace(start=target, join=Mock())

            with (
                self.subTest(condition=condition),
                patch.object(ta, "InputStream", return_value=capture),
                patch.object(ta, "ScheduledOutputStream", return_value=output),
                patch.object(report.threading, "Event", return_value=event),
                patch.object(report.threading, "Thread", side_effect=thread),
                patch.object(report.mp, "Process") as worker,
                patch.object(report.time, "sleep"),
                patch.object(report, "_detect", return_value=(indices, np.ones(3))),
            ):
                if condition != "valid":
                    with self.assertRaises(report.AudioReportError):
                        report._measure("out", "in", 0.001, 3, True)
                else:
                    result = report._measure("out", "in", 0.001, 3, True)
                    self.assertTrue(result["detections_valid"])
                    self.assertEqual(result["condition"], "cpu-load")
                    self.assertEqual(result["scheduler_onset_error_us_max"], 1)
                    self.assertLess(result["jitter_max_us"], 25)
                    self.assertEqual(result["input_hardware_latency_ms"], 2)
                self.assertEqual(worker.return_value.terminate.call_count, 4)
                self.assertEqual(worker.return_value.join.call_count, 4)


class ReportCommandTests(unittest.TestCase):
    def test_report_modes_keep_failed_scans_and_recommendations(self):
        for platform, mode, recommended in (
            ("darwin", "full", True),
            ("linux", "tune", False),
            ("darwin", "immediate", False),
        ):
            immediate = dict(
                rows=[
                    dict(
                        latency_s=0.001,
                        load_workers=0,
                        failures=0,
                        onset_median_ms=1.0,
                        onset_p99_ms=2.0,
                    ),
                    dict(latency_s=0.002, load_workers=4, failures=1, error="unavailable"),
                ],
                recommendation=dict(latency_s=0.001, loaded_p99_ms=2.0) if recommended else None,
            )
            tuning = dict(
                rows=[
                    dict(
                        latency_s=0.001,
                        default_lead_s=0.01,
                        requested_lead_s=None,
                        load_workers=0,
                        passed=True,
                    ),
                    dict(latency_s=0.001, requested_lead_s=0.005, load_workers=0, passed=True),
                    dict(latency_s=0.001, requested_lead_s=0.005, load_workers=4, passed=True),
                    dict(latency_s=0.002, requested_lead_s=None, load_workers=0, passed=False),
                ],
                validation=[],
                recommendation=dict(latency_s=0.001, lead_time_s=0.005) if recommended else None,
            )
            args = SimpleNamespace(
                output_device="out",
                input_device="in",
                tune_only=mode == "tune",
                immediate_only=mode == "immediate",
                latency=0.001,
                skip_lead_scan=False,
                lead_trials=2,
                validation_trials=3,
                load_workers=4,
                stimulus_ms=1,
                trials=3,
            )
            with (
                self.subTest(mode=mode),
                patch.object(report, "sys", SimpleNamespace(platform=platform)),
                patch.object(report, "_device_name", side_effect=lambda device: device),
                patch.object(_lead_probe, "scan_immediate", return_value=immediate),
                patch.object(_lead_probe, "scan_lead_times", return_value=tuning),
                patch.object(report, "_check_microphone"),
                patch.object(report, "_measure", return_value=measurement()),
                contextlib.redirect_stdout(io.StringIO()) as printed,
            ):
                result = report._run(args)
            self.assertIn("immediate_scan", result)
            if mode == "full":
                self.assertEqual(result["idle"], measurement())
                self.assertIn("low confidence", printed.getvalue())
            elif mode == "tune":
                self.assertIsNone(result["lead_scan"]["recommendation"])

    def test_command_writes_json_and_supports_default_device_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested" / "report.json"
            with (
                patch.object(report, "sys", SimpleNamespace(platform="darwin")),
                patch.object(report, "_run", return_value={"validated": True}),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                report.main(["--tune-only", "--json", str(path)])
            self.assertEqual(json.loads(path.read_text()), {"validated": True})
        args = SimpleNamespace(
            output_device=None,
            input_device=None,
            tune_only=False,
            immediate_only=False,
            latency=0.001,
            skip_lead_scan=True,
            trials=3,
        )
        with (
            patch.object(report, "_default_device", side_effect=["out", "in"]),
            patch.object(report, "_device_name", side_effect=lambda value: value),
            patch.object(report, "_check_microphone"),
            patch.object(report, "_measure", return_value=measurement()),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(report._run(args)["input_device"], "in")

    def test_invalid_command_settings_fail_before_opening_audio(self):
        for args in (
            ["--latency", "0"],
            ["--trials", "2"],
            ["--stimulus-ms", "0"],
            ["--load-workers", "0"],
            ["--immediate-only", "--skip-lead-scan"],
        ):
            with (
                self.subTest(args=args),
                patch.object(report, "sys", SimpleNamespace(platform="darwin")),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as error,
            ):
                report.main(args)
            self.assertEqual(error.exception.code, 2)
        with (
            patch.object(report, "sys", SimpleNamespace(platform="win32")),
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            report.main([])
        with (
            patch.object(report, "sys", SimpleNamespace(platform="darwin")),
            patch.object(report, "np", None),
            self.assertRaisesRegex(SystemExit, "needs numpy"),
        ):
            report.main([])
        with (
            patch.object(report, "sys", SimpleNamespace(platform="darwin")),
            patch.object(report, "_run", side_effect=report.AudioReportError("no device")),
            self.assertRaisesRegex(SystemExit, "no device"),
        ):
            report.main([])
