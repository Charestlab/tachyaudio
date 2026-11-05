"""Headless public stream, native adapter, and failure-lifecycle contracts."""

import unittest
from array import array
from unittest.mock import Mock, patch

import tachyaudio as ta
from tachyaudio import _backend, _stream
from tachyaudio import _native_backend as native


def stats():
    return dict(
        frames_processed=12,
        underruns=1,
        overruns=2,
        estimated_latency=0.01,
        hardware_latency=0.002,
        queued_frames=3,
        queued_latency=0.003,
        buffer_size=256,
    )


class NativeAdapterTests(unittest.TestCase):
    def test_native_devices_preserve_defaults_and_round_rates(self):
        rows = [
            dict(
                id="out",
                name="Speaker",
                kind="output",
                channels=2,
                default_sample_rate=48000.4,
                is_default=True,
            ),
            dict(
                id="in",
                name="Mic",
                kind="input",
                channels=1,
                default_sample_rate=0,
                is_default=False,
            ),
        ]
        with patch.object(native._native, "list_devices", return_value=rows):
            devices = native.NativeBackend().list_devices()
        self.assertEqual([d.default_sample_rate for d in devices], [48000, None])
        self.assertEqual(devices[1].kind, ta.DeviceKind.INPUT)

    def test_output_adapter_owns_handle_and_preserves_stats(self):
        handle = Mock()
        handle.write.return_value = 2
        handle.drain.return_value = True
        handle.stats.return_value = stats()
        config = ta.StreamConfig(sample_rate=44100, channels=1, device_id="out", latency=0.02)
        with patch.object(native._native, "OutputStream", return_value=handle) as factory:
            output = native.NativeBackend().open_output_stream(config)
        factory.assert_called_once_with(44100, 1, 0, "out", 0.02)
        output.start()
        self.assertEqual(output.write(b"samples"), 2)
        self.assertTrue(output.drain())
        handle.drain.assert_called_with(-1)
        output.drain(0.1)
        handle.drain.assert_called_with(0.1)
        self.assertEqual(output.stats(), ta.StreamStats(**stats()))
        output.flush()
        output.stop()
        output.close()
        for method in ("start", "flush", "stop", "close"):
            getattr(handle, method).assert_called_once()

    def test_input_and_duplex_adapters_preserve_frames_and_configuration(self):
        handle = Mock()
        handle.read.return_value = b"\0" * 8
        handle.write.return_value = 2
        handle.stats.return_value = stats()
        with patch.object(native._native, "InputStream", return_value=handle) as factory:
            capture = native.NativeBackend().open_input_stream(ta.StreamConfig(block_size=64))
        factory.assert_called_once_with(48000, 2, 64, None, 0)
        self.assertEqual(bytes(capture.read(1)), b"\0" * 8)
        self.assertEqual(capture.stats(), ta.StreamStats(**stats()))
        for method in ("start", "stop", "flush", "close"):
            getattr(capture, method)()
            getattr(handle, method).assert_called_once()
        handle.reset_mock()
        handle.stats.return_value = {"input": stats(), "output": stats()}
        backend = native.NativeBackend()
        backend.name = "coreaudio"
        with patch.object(
            native._native, "DuplexStream", return_value=handle, create=True
        ) as factory:
            duplex = backend.open_duplex_stream(ta.DuplexStreamConfig(input_device_id="mic"))
        factory.assert_called_once_with(48000, 1, 2, 0, "mic", None, 0)
        self.assertEqual(bytes(duplex.read(2)), b"\0" * 8)
        self.assertEqual(duplex.write(b"samples"), 2)
        self.assertEqual(duplex.stats().input, ta.StreamStats(**stats()))
        for method in ("start", "stop", "flush", "close"):
            getattr(duplex, method)()
            getattr(handle, method).assert_called_once()
        backend.name = "unsupported"
        with self.assertRaises(ta.BackendUnavailable):
            backend.open_duplex_stream(ta.DuplexStreamConfig())


class StreamFailureTests(unittest.TestCase):
    def tearDown(self):
        ta.set_backend(None)

    def test_missing_backend_has_actionable_failures(self):
        with patch.dict("sys.modules", {"tachyaudio._native_backend": None}):
            unavailable = _backend._load_default_backend()
        self.assertEqual(unavailable.list_devices(), ())
        for kind in ("output", "input", "duplex"):
            with self.subTest(kind=kind), self.assertRaises(ta.BackendUnavailable):
                getattr(unavailable, f"open_{kind}_stream")(None)

    def test_context_start_failure_closes_each_stream(self):
        for stream_class, method in (
            (ta.OutputStream, "open_output_stream"),
            (ta.InputStream, "open_input_stream"),
            (ta.DuplexStream, "open_duplex_stream"),
        ):
            with self.subTest(stream=stream_class.__name__):
                handle, backend = Mock(), Mock()
                handle.start.side_effect = RuntimeError("start failed")
                getattr(backend, method).return_value = handle
                ta.set_backend(backend)
                stream = stream_class()
                with self.assertRaisesRegex(RuntimeError, "start failed"), stream:
                    self.fail("a failed start cannot enter the context")
                handle.close.assert_called_once()
                self.assertTrue(stream.closed)

    def test_capture_and_duplex_close_are_idempotent_and_guard_operations(self):
        for stream_class, method in (
            (ta.InputStream, "open_input_stream"),
            (ta.DuplexStream, "open_duplex_stream"),
        ):
            handle, backend = Mock(), Mock()
            getattr(backend, method).return_value = handle
            ta.set_backend(backend)
            with stream_class() as stream:
                stream.stop()
                stream.flush()
                with self.assertRaises(ValueError):
                    stream.read(0)
            stream.close()
            handle.close.assert_called_once()
            self.assertTrue(stream.closed)
            for operation in (
                stream.start,
                stream.stop,
                stream.flush,
                stream.stats,
                lambda: stream.read(1),
                lambda: stream.read_exactly(1),
            ):
                with self.assertRaises(ta.StreamClosed):
                    operation()

    def test_write_and_capture_reject_invalid_backend_counts(self):
        for accepted in (-1, 3):
            with self.subTest(accepted=accepted), self.assertRaises(RuntimeError):
                _stream._write_all_to_handle(
                    lambda data, count=accepted: count, b"\0" * 8, channels=1, timeout=None
                )
        for chunk in (b"x", b"\0" * 12):
            with self.subTest(chunk=chunk), self.assertRaises(RuntimeError):
                _stream._read_exactly_from_handle(
                    lambda count, data=chunk: memoryview(data), 2, channels=1, timeout=None
                )
        with self.assertRaises(ValueError):
            _stream._read_exactly_from_handle(Mock(), 0, channels=1, timeout=None)
        with self.assertRaises(ValueError):
            _stream._validate_frames(array("f"), 1)

    def test_play_failure_always_closes_handle(self):
        for accepted, drained, error in (
            (-1, True, RuntimeError),
            (3, True, RuntimeError),
            (2, False, TimeoutError),
        ):
            handle, backend = Mock(), Mock()
            handle.write.return_value = accepted
            handle.drain.return_value = drained
            backend.open_output_stream.return_value = handle
            ta.set_backend(backend)
            with self.subTest(accepted=accepted), self.assertRaises(error):
                ta.play(array("f", [0, 0]), channels=1)
            handle.close.assert_called_once()

    def test_negative_stats_and_stream_options_rejected(self):
        for field in (
            "frames_processed",
            "underruns",
            "overruns",
            "estimated_latency",
            "hardware_latency",
            "queued_frames",
            "queued_latency",
            "buffer_size",
        ):
            with self.subTest(field=field), self.assertRaises(ValueError):
                ta.StreamStats(**{field: -1})
        for options in ({"block_size": 0}, {"latency": 0}):
            with self.assertRaises(ValueError):
                ta.StreamConfig(**options)
        with self.assertRaises(ValueError):
            ta.DeviceInfo(
                id="x", name="x", kind=ta.DeviceKind.OUTPUT, channels=1, default_sample_rate=0
            )
