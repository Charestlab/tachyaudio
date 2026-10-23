"""Production Core Audio queue/renderer exercised without an audio device."""
import sys
import time
import unittest
from array import array


@unittest.skipUnless(sys.platform == 'darwin', 'Core Audio extension')
class CoreQueueTests(unittest.TestCase):
    def setUp(self):
        from tachyaudio import _native
        self.stream = _native._scheduled_test_stream(1, 2)
        self.addCleanup(self.stream.close)
        self.begin = time.monotonic_ns() + 2_000_000_000

    def submit(self, values, offset=0):
        return self.stream.schedule(array('f', values), self.begin + round(offset * 1e9 / 48000), 0)

    def render(self, frames=8, callbacks=3, gap=False):
        return [list(memoryview(b).cast('f')) for b in
                self.stream._render_test(self.begin, frames, callbacks, gap)]

    def test_immediate_uses_first_callback_even_after_submission_time(self):
        submitted = time.monotonic_ns()
        ident = self.stream.schedule(array('f', [.25]*10), submitted, 0, True)
        buffers = self.render()
        self.assertEqual(buffers[0], [.25]*16)
        self.assertEqual(buffers[1], [.25]*4 + [0]*12)
        result = self.stream.result(ident)
        self.assertTrue(result['immediate'])
        self.assertEqual(result['requested_ns'], submitted)
        self.assertAlmostEqual(result['predicted_onset_ns'], self.begin, delta=42)
        self.assertEqual(result['state'], 4)

    def test_immediate_reserves_pending_and_rendered_audio(self):
        ident = self.stream.schedule(array('f', [.25]), time.monotonic_ns(), 0, True)
        for rendered in (False, True):
            if rendered:
                self.render(callbacks=1)
            with self.assertRaisesRegex(ValueError, 'idle stream'):
                self.stream.schedule(array('f', [.5]), time.monotonic_ns(), 0, True)
            with self.assertRaisesRegex(ValueError, 'idle stream'):
                self.submit([.5], 100)
        self.assertEqual(self.stream.result(ident)['state'], 4)

    def test_immediate_does_not_displace_a_scheduled_stimulus(self):
        self.submit([.25], 100)
        with self.assertRaisesRegex(ValueError, 'idle stream'):
            self.stream.schedule(array('f', [.5]), time.monotonic_ns(), 0, True)

    def test_pending_immediate_cancellation_leaves_silence(self):
        ident = self.stream.schedule(array('f', [.25]), time.monotonic_ns(), 0, True)
        self.stream.cancel(ident)
        self.assertTrue(all(v == 0 for b in self.render() for v in b))
        self.assertEqual(self.stream.result(ident)['state'], 5)

    def test_queue_back_to_back_in_same_buffer_mono_duplicates(self):
        first = self.submit([.25] * 4)
        second = self.submit([.5] * 4, 4)
        buffers = self.render()
        self.assertEqual(buffers[0], [.25] * 8 + [.5] * 8)
        self.assertTrue(all(v == 0 for v in buffers[1]))
        for ident in (first, second):
            self.assertEqual(self.stream.result(ident)['state'], 4)
            self.assertEqual(self.stream.result(ident)['rendered_frames'], 4)
        self.assertEqual(self.stream.status()['queued'], 0)

    def test_out_of_order_submission_and_continuation(self):
        second = self.submit([.5] * 5, 10)
        first = self.submit([.25] * 10)
        flat = sum(self.render(), [])
        self.assertEqual(flat[:30], [.25] * 20 + [.5] * 10)
        self.assertEqual(self.stream.result(first)['state'], 4)
        self.assertEqual(self.stream.result(second)['state'], 4)

    def test_missed_and_cancelled_do_not_poison_other_stimuli(self):
        missed = self.submit([.25], -4)
        cancelled = self.submit([.5], 2)
        kept = self.submit([.75], 4)
        self.stream.cancel(cancelled)
        buffers = self.render()
        self.assertEqual(buffers[0], [0] * 8 + [.75] * 2 + [0] * 6)
        self.assertEqual(self.stream.result(missed)['state'], 6)
        self.assertEqual(self.stream.result(cancelled)['state'], 5)
        self.assertEqual(self.stream.result(kept)['state'], 4)
        self.assertEqual(self.stream.status()['state'], 4)

    def test_capacity_invalid_samples_and_unknown_id(self):
        self.submit([.25] * 4)
        for offset in range(1, 32):
            self.submit([0], offset * 10)
        with self.assertRaisesRegex(ValueError, 'too many'):
            self.submit([0], 330)
        with self.assertRaisesRegex(ValueError, 'finite'):
            self.submit([float('nan')], 340)
        with self.assertRaises(LookupError):
            self.stream.result(99)

    def test_discontinuity_fails_all_pending_and_requires_reopen(self):
        first = self.submit([.25] * 20)
        second = self.submit([.5], 30)
        self.render(gap=True)
        for ident in (first, second):
            self.assertEqual(self.stream.result(ident)['state'], 8)
        self.assertEqual(self.stream.status()['state'], 8)
        with self.assertRaises(RuntimeError):
            self.submit([0], 40)
        with self.assertRaises(RuntimeError):
            self.stream.start()

    def test_close_cancels_all_pending_records(self):
        ids = [self.submit([0], n * 10) for n in range(3)]
        self.stream.close()
        for ident in ids:
            self.assertEqual(self.stream.result(ident)['state'], 5)
        self.stream.close()

    def test_overlapping_stimuli_mix(self):
        a = self.submit([.25] * 4)        # frames 0-3
        b = self.submit([.5] * 4)         # same onset: overlaps a fully
        buffers = self.render()
        # mono duplicated to 2 device channels; the two stimuli sum to .75
        self.assertEqual(buffers[0], [.75] * 8 + [0] * 8)
        for ident in (a, b):
            self.assertEqual(self.stream.result(ident)['state'], 4)

    def test_overlapping_mix_is_clamped(self):
        self.submit([.8] * 4)
        self.submit([.8] * 4)             # same onset: sum 1.6 clamps to 1.0
        buffers = self.render()
        self.assertEqual(buffers[0], [1.0] * 8 + [0] * 8)

    def test_overlap_allowed_after_render(self):
        self.submit([.25] * 4)
        self.render(callbacks=1)
        # previously rejected as an overlap; now accepted (mixed)
        self.assertIsNotNone(self.submit([.5], 1))

    def test_diagnostics_observe_device_timestamps(self):
        self.render()
        status = self.stream.status()
        self.assertEqual(status['clock_observations'], 3)
        self.assertEqual(status['clock_source'], 'coreaudio-output-host-time')
        self.assertGreater(status['output_delay_max_us'], 0)

    def test_cancel_all_releases_queue_capacity_after_callback(self):
        for offset in range(32):
            self.submit([0], 1000 + offset * 10)
        self.stream.cancel()
        self.render(callbacks=1)
        self.assertEqual(self.stream.status()['queued'], 0)
        ident = self.submit([.5], 2000)
        self.assertEqual(ident, 33)
        with self.assertRaises(LookupError):
            self.stream.result(1)

    def test_stereo_preserves_independent_channels(self):
        from tachyaudio import _native
        stream = _native._scheduled_test_stream(2, 2)
        try:
            ident = stream.schedule(array('f', [.25, -.5] * 4), self.begin, 0)
            buffers = stream._render_test(self.begin, 8, 1)
            self.assertEqual(list(memoryview(buffers[0]).cast('f')), [.25, -.5] * 4 + [0] * 8)
            self.assertEqual(stream.result(ident)['state'], 4)
        finally:
            stream.close()
