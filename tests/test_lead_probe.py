"""Recommendations require independent idle and loaded validation."""

import unittest
from unittest.mock import patch

from tachyaudio._lead_probe import scan_lead_times


class LeadScanTests(unittest.TestCase):
    @staticmethod
    def probe(device, rate, latency, lead, trials, workers):
        return {
            "latency_s": latency,
            "requested_lead_s": lead,
            "default_lead_s": 0.021,
            "load_workers": workers,
            "trials": [],
            "passed": lead is None or lead >= (0.020 if workers else 0.018),
        }

    def test_recommends_loaded_threshold_not_idle_minimum(self):
        with patch("tachyaudio._lead_probe._probe", side_effect=self.probe):
            result = scan_lead_times("device", buffers=(0.001,), progress=lambda _: None)
        self.assertAlmostEqual(result["recommendation"]["lead_time_s"], 0.020)
        self.assertEqual(len(result["validation"]), 2)
        self.assertTrue(any(not row["passed"] for row in result["rows"]))

    def test_no_recommendation_when_fresh_validation_fails(self):
        def probe(*args):
            row = self.probe(*args)
            if args[4] == 200:
                row["passed"] = False
            return row

        with patch("tachyaudio._lead_probe._probe", side_effect=probe):
            result = scan_lead_times("device", buffers=(0.001,), progress=lambda _: None)
        self.assertIsNone(result["recommendation"])
        self.assertTrue(result["validation"])

    def test_unavailable_device_is_retained_and_never_recommended(self):
        with patch(
            "tachyaudio._lead_probe._probe", return_value={"passed": False, "error": "unavailable"}
        ):
            result = scan_lead_times("device", buffers=(0.001,), progress=lambda _: None)
        self.assertIsNone(result["recommendation"])
        self.assertEqual(result["rows"][0]["error"], "unavailable")


class ImmediateScanTests(unittest.TestCase):
    def test_ranks_loaded_p99_and_rejects_failed_confirmation(self):
        from tachyaudio._lead_probe import scan_immediate
        def probe(device, rate, latency, lead, trials, workers, **kwargs):
            return {"passed": not (latency == .001 and trials == 200),
                    "latency_s": latency, "onset_p99_ms": latency * 1000,
                    "load_workers": workers}
        with patch("tachyaudio._lead_probe.ta.ScheduledOutputStream") as factory, \
                patch("tachyaudio._lead_probe._probe", side_effect=probe):
            factory.return_value.__enter__.return_value.status.return_value = {}
            result = scan_immediate("device", buffers=(.001, .002), progress=lambda _: None)
        self.assertEqual(result["recommendation"]["latency_s"], .002)
        self.assertEqual(len(result["validation"]), 4)

    def test_includes_hardware_minimum_and_keeps_failures(self):
        from tachyaudio._lead_probe import scan_immediate
        with patch("tachyaudio._lead_probe.ta.ScheduledOutputStream") as factory, \
                patch("tachyaudio._lead_probe._probe", return_value={"passed": False}) as probe:
            factory.return_value.__enter__.return_value.status.return_value = {
                "minimum_buffer_frames": 15}
            result = scan_immediate("device", buffers=(.001,), progress=lambda _: None)
        self.assertTrue(any(call.args[2] == .000312 for call in probe.call_args_list))
        self.assertIsNone(result["recommendation"])
        self.assertEqual(len(result["rows"]), 4)


class PlatformImmediateTests(unittest.TestCase):
    def test_linux_measures_actual_guarded_play_now_path(self):
        from unittest.mock import Mock

        from tachyaudio._lead_probe import _submit_immediate
        stream = Mock()
        stream.lead_time = .020
        stream.schedule.return_value = 7
        with patch('tachyaudio._lead_probe.sys.platform', 'linux'), \
                patch('tachyaudio._lead_probe.time.monotonic', return_value=10):
            self.assertEqual(_submit_immediate(stream, b'audio'), 7)
        stream.schedule.assert_called_once_with(b'audio', when=10.025)
        stream.schedule_asap.assert_not_called()

    def test_mac_uses_callback_instead_of_a_python_deadline(self):
        from unittest.mock import Mock

        from tachyaudio._lead_probe import _submit_immediate
        stream = Mock()
        stream.schedule_asap.return_value = 8
        with patch('tachyaudio._lead_probe.sys.platform', 'darwin'):
            self.assertEqual(_submit_immediate(stream, b'audio'), 8)
        stream.schedule.assert_not_called()
        stream.schedule_asap.assert_called_once_with(b'audio')

    def test_linux_cli_accepts_immediate_and_requires_direct_hardware(self):
        from tachyaudio.audio_report import main
        with patch('tachyaudio.audio_report.sys.platform', 'linux'), \
                patch('tachyaudio.audio_report._run', return_value={}) as run:
            main(['--immediate-only', '--output-device', 'hw:0,0'])
            self.assertTrue(run.call_args.args[0].immediate_only)
            with self.assertRaises(SystemExit):
                main(['--tune-only', '--output-device', 'default'])
