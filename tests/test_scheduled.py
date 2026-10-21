from __future__ import annotations

import sys
import time
import unittest
from array import array
from concurrent.futures import CancelledError
from unittest.mock import Mock, patch

import tachyaudio as ta


class ScheduledApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.handle = Mock()
        native = Mock()
        native.ScheduledOutputStream.return_value = self.handle
        self.patch = patch.dict(sys.modules, {"tachyaudio._native": native})
        self.patch.start()
        self.attribute_patch = patch.object(ta, "_native", native, create=True)
        self.attribute_patch.start()
        self.platform_patch = patch("tachyaudio._scheduled.sys.platform", "darwin")
        self.platform_patch.start()
        self.stream = ta.ScheduledOutputStream(channels=1)

    def tearDown(self) -> None:
        self.stream.close()
        self.attribute_patch.stop()
        self.patch.stop()
        self.platform_patch.stop()

    def test_schedule_submits_data_and_deadline_without_waiting_for_onset(self) -> None:
        target = time.monotonic() + 60
        self.stream.schedule(array("f", [0.1]), when=target)
        view, actual_target, lead = self.handle.schedule.call_args.args
        self.assertEqual(view.tobytes(), array("f", [0.1]).tobytes())
        self.assertEqual(actual_target, round(target * 1e9))
        self.assertEqual(lead, 50_000_000)

    def test_late_deadline_is_rejected_without_submitting(self) -> None:
        with self.assertRaises(ta.SchedulingError):
            self.stream.schedule(array("f", [0.0]), when=time.monotonic() + 0.001)
        self.handle.schedule.assert_not_called()

    def test_invalid_clock_values_are_rejected(self) -> None:
        for when in (float("nan"), float("inf"), -1, 1e100):
            with self.subTest(when=when), self.assertRaises(ValueError):
                self.stream.schedule(array("f", [0.0]), when=when)

    def test_native_failures_and_cancellation_are_observable(self) -> None:
        for state in range(6, 11):
            self.handle.status.return_value = {"state": state}
            with self.subTest(state=state), self.assertRaises(ta.SchedulingError):
                self.stream.wait(timeout=0)
        self.handle.status.return_value = {"state": 5}
        with self.assertRaises(CancelledError):
            self.stream.wait()

    def test_wait_timeout_does_not_cancel(self) -> None:
        self.handle.status.return_value = {"state": 2}
        with self.assertRaises(TimeoutError):
            self.stream.wait(timeout=0)
        self.handle.cancel.assert_not_called()

    def test_render_result_is_not_reported_as_physical_onset(self) -> None:
        self.handle.status.return_value = {
            "state": 4, "requested_ns": 1_000_000_000,
            "predicted_onset_ns": 1_000_010_000, "render_callback_ns": 980_000_000,
            "render_complete_ns": 980_001_000,
            "predicted_end_ns": 1_010_010_000, "rendered_frames": 480,
        }
        result = self.stream.wait()
        self.assertEqual(result.predicted_error_seconds, 0.00001)
        self.assertEqual(result.rendered_frames, 480)

    def test_close_is_idempotent_and_prevents_schedule(self) -> None:
        self.stream.close()
        self.stream.close()
        self.handle.close.assert_called_once()
        with self.assertRaises(ta.StreamClosed):
            self.stream.schedule(array("f", [0.0]), when=time.monotonic() + 1)

    def test_wait_keeps_device_open_through_predicted_completion(self) -> None:
        self.handle.status.return_value = {
            "state": 4, "requested_ns": 1_000_000_000,
            "predicted_onset_ns": 1_000_010_000, "render_callback_ns": 980_000_000,
            "render_complete_ns": 980_001_000, "predicted_end_ns": 1_010_010_000,
            "rendered_frames": 480, "buffer_frames": 480,
        }
        with patch("tachyaudio._scheduled.time.monotonic_ns", side_effect=[
            1_010_010_000, 1_020_010_000
        ]), patch("tachyaudio._scheduled.time.sleep") as sleep:
            result = self.stream.wait()
        sleep.assert_called_once()
        self.assertEqual(result.rendered_frames, 480)
        self.handle.close.assert_not_called()


class LinuxDefaultsTests(unittest.TestCase):
    def test_missing_hw_device_lists_available_devices(self) -> None:
        with patch("tachyaudio._scheduled.sys.platform", "linux"), \
                patch("tachyaudio._alsa_devices.describe_alsa_outputs",
                      return_value="DEVICE TABLE"):
            for device in (None, "default", "pulse"):
                with self.subTest(device=device), \
                        self.assertRaisesRegex(ValueError, "device_id(.|\n)*DEVICE TABLE"):
                    ta.ScheduledOutputStream(device_id=device)

    def test_queue_ids_reach_native_wait_and_cancel(self) -> None:
        handle = Mock()
        handle.status.return_value = {"output_delay_max_us": 0.0, "period_frames": 48,
                                      "buffer_frames": 0, "state": 4}
        handle.schedule.side_effect = [1, 2]
        record = {"state": 4, "requested_ns": 1, "predicted_onset_ns": 1,
                  "render_callback_ns": 0, "render_complete_ns": 0,
                  "predicted_end_ns": 1, "rendered_frames": 1}
        handle.result.return_value = record
        native = Mock()
        native.ScheduledOutputStream.return_value = handle
        with patch.dict(sys.modules, {"tachyaudio._scheduled_alsa": native}), \
                patch.object(ta, "_scheduled_alsa", native, create=True), \
                patch("tachyaudio._scheduled.sys.platform", "linux"):
            stream = ta.ScheduledOutputStream(device_id="hw:0,0")
            with self.assertRaises(ta.SchedulingError):
                stream.wait_rendered(timeout=0)
            first = stream.schedule(array("f", [0.0, 0.0]), when=time.monotonic() + 1)
            second = stream.schedule(array("f", [0.0, 0.0]), when=time.monotonic() + 2)
            self.assertEqual((first, second), (1, 2))
            stream.wait(timeout=1, stimulus=first)
            handle.result.assert_called_with(1)
            stream.wait_rendered(timeout=1)
            handle.result.assert_called_with(0)
            stream.cancel(second)
            handle.cancel.assert_called_with(2)
            stream.cancel()
            handle.cancel.assert_called_with(0)
            self.assertFalse(stream.failed)

    @unittest.skipUnless(sys.platform.startswith("linux"), "reads /proc/asound")
    def test_device_listing_is_well_formed(self) -> None:
        for device in ta.list_alsa_outputs():
            self.assertRegex(device.device_id, r"^hw:CARD=[^,]+,DEV=\d+$")
            self.assertRegex(device.short_id, r"^hw:\d+,\d+$")
            self.assertIn(device.kind, ("screen", "jacks", "digital", "USB", "other"))
            self.assertIn(device.device_id, str(device))

    def test_lead_time_follows_measured_output_delay_unless_given(self) -> None:
        handle = Mock()
        handle.status.return_value = {"output_delay_max_us": 10_000.0, "period_frames": 240}
        native = Mock()
        native.ScheduledOutputStream.return_value = handle
        with patch.dict(sys.modules, {"tachyaudio._scheduled_alsa": native}), \
                patch.object(ta, "_scheduled_alsa", native, create=True), \
                patch("tachyaudio._scheduled.sys.platform", "linux"):
            stream = ta.ScheduledOutputStream(device_id="hw:0,0")
            native.ScheduledOutputStream.assert_called_once_with(48_000, 2, "hw:0,0")
            self.assertAlmostEqual(stream.lead_time, 0.010 + 0.005 + 0.002)
            with self.assertRaises(ta.SchedulingError):
                stream.schedule(array("f", [0.0, 0.0]), when=time.monotonic() + 0.012)
            stream.schedule(array("f", [0.0, 0.0]), when=time.monotonic() + 0.050)
            self.assertEqual(handle.schedule.call_args.args[2], 17_000_000)
            fixed = ta.ScheduledOutputStream(device_id="hw:0,0", minimum_lead_time=0.1,
                                             latency=0.02)
            self.assertEqual(fixed.lead_time, 0.1)
            self.assertEqual(native.ScheduledOutputStream.call_args.args,
                             (48_000, 2, "hw:0,0", 20_000))


@unittest.skipUnless(sys.platform == "darwin", "macOS native renderer")
class NativeRendererTests(unittest.TestCase):
    def render(self, samples, offset, *, frames=8, callbacks=3, gap=False):
        from tachyaudio import _native

        beginning = time.monotonic_ns() + 1_000_000_000
        target = beginning + round(offset * 1e9 / 48000)
        buffers, status = _native._scheduled_render_test(
            array("f", samples), 1, target, beginning, frames, callbacks, gap
        )
        return [list(memoryview(buffer).cast("f")) for buffer in buffers], status

    def test_places_samples_inside_buffer_and_preserves_continuation(self) -> None:
        buffers, status = self.render([0.25, 0.5, 0.75, 1], 6.25)
        self.assertEqual(buffers[0], [0] * 7 + [0.25])
        self.assertEqual(buffers[1], [0.5, 0.75, 1] + [0] * 5)
        self.assertEqual(buffers[2], [0] * 8)
        self.assertEqual(status["state"], 4)
        self.assertEqual(status["rendered_frames"], 4)

    def test_future_stimulus_leaves_earlier_buffer_silent(self) -> None:
        buffers, status = self.render([1], 10.25)
        self.assertEqual(buffers[0], [0] * 8)
        self.assertEqual(buffers[1], [0] * 3 + [1] + [0] * 4)
        self.assertEqual(status["state"], 4)

    def test_late_stimulus_is_dropped_not_shifted(self) -> None:
        buffers, status = self.render([1], -2)
        self.assertTrue(all(buffer == [0] * 8 for buffer in buffers))
        self.assertEqual(status["state"], 6)
        self.assertEqual(status["rendered_frames"], 0)

    def test_rounding_across_buffer_boundary_uses_first_sample_of_next_buffer(self) -> None:
        buffers, status = self.render([1], 7.75)
        self.assertEqual(buffers[0], [0] * 8)
        self.assertEqual(buffers[1], [1] + [0] * 7)
        self.assertEqual(status["state"], 4)
        error_ns = status["predicted_onset_ns"] - status["requested_ns"]
        self.assertGreaterEqual(error_ns, 0)
        self.assertLessEqual(error_ns, 1e9 / 48000)

    def test_sample_clock_discontinuity_stops_remaining_audio(self) -> None:
        buffers, status = self.render([1] * 12, 0.25, gap=True)
        self.assertEqual(buffers[0], [0] + [1] * 7)
        self.assertEqual(buffers[1], [0] * 8)
        self.assertEqual(status["state"], 8)
