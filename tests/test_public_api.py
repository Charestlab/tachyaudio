from __future__ import annotations

import unittest

import tachyaudio as ta


class PublicApiTests(unittest.TestCase):
    def tearDown(self) -> None:
        ta.set_backend(None)

    def test_version_is_exposed(self) -> None:
        self.assertIsInstance(ta.__version__, str)

    def test_default_backend_lists_no_devices(self) -> None:
        self.assertIsInstance(ta.list_devices(), tuple)

    def test_custom_backend_can_reject_output_stream(self) -> None:
        class Backend:
            name = "test"

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise ta.BackendUnavailable("test backend unavailable")

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        ta.set_backend(Backend())
        with self.assertRaises(ta.BackendUnavailable):
            ta.OutputStream()

    def test_stream_config_validates_values(self) -> None:
        with self.assertRaises(ValueError):
            ta.StreamConfig(sample_rate=0)
        with self.assertRaises(ValueError):
            ta.StreamConfig(channels=0)
        with self.assertRaises(ValueError):
            ta.StreamConfig(dtype="int16")

    def test_duplex_stream_config_validates_values(self) -> None:
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(sample_rate=0)
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(input_channels=0)
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(output_channels=0)
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(block_size=0)
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(latency=0)
        with self.assertRaises(ValueError):
            ta.DuplexStreamConfig(dtype="int16")

    def test_device_info_validates_values(self) -> None:
        with self.assertRaises(ValueError):
            ta.DeviceInfo(id="", name="Speakers", kind=ta.DeviceKind.OUTPUT, channels=2)
        with self.assertRaises(ValueError):
            ta.DeviceInfo(id="default", name="", kind=ta.DeviceKind.OUTPUT, channels=2)
        with self.assertRaises(ValueError):
            ta.DeviceInfo(id="default", name="Speakers", kind=ta.DeviceKind.OUTPUT, channels=0)

    def test_custom_backend_can_list_devices(self) -> None:
        class Backend:
            name = "test"

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return (
                    ta.DeviceInfo(
                        id="output-0",
                        name="Test Output",
                        kind=ta.DeviceKind.OUTPUT,
                        channels=2,
                        default_sample_rate=48_000,
                        is_default=True,
                    ),
                )

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        ta.set_backend(Backend())
        devices = ta.list_devices()
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0].name, "Test Output")

    def test_output_stream_lifecycle_delegates_to_backend(self) -> None:
        class OutputHandle:
            def __init__(self) -> None:
                self.started = False
                self.closed = False
                self.frames_processed = 0
                self.queued_frames = 0

            def start(self) -> None:
                self.started = True

            def stop(self) -> None:
                self.started = False

            def drain(self, timeout: float | None = None) -> bool:
                return True

            def flush(self) -> None:
                self.queued_frames = 0

            def close(self) -> None:
                self.closed = True

            def write(self, frames: object) -> int:
                self.frames_processed = 12
                self.queued_frames = 12
                return 12

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats(
                    frames_processed=self.frames_processed,
                    queued_frames=self.queued_frames,
                )

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = OutputHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> OutputHandle:
                return self.handle

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        backend = Backend()
        ta.set_backend(backend)

        stream = ta.OutputStream()
        stream.start()
        self.assertTrue(backend.handle.started)
        self.assertEqual(stream.write(b"frames"), 12)
        self.assertTrue(stream.drain(timeout=0.1))
        self.assertEqual(stream.stats().frames_processed, 12)
        stream.flush()
        self.assertEqual(stream.stats().queued_frames, 0)
        stream.close()
        self.assertTrue(stream.closed)
        self.assertTrue(backend.handle.closed)

        with self.assertRaises(ta.StreamClosed):
            stream.write(b"frames")

    def test_play_helper_returns_stats(self) -> None:
        class OutputHandle:
            def __init__(self) -> None:
                self.events: list[str] = []
                self.frames = 0

            def start(self) -> None:
                self.events.append("start")

            def stop(self) -> None:
                pass

            def drain(self, timeout: float | None = None) -> bool:
                self.events.append("drain")
                return True

            def flush(self) -> None:
                pass

            def close(self) -> None:
                pass

            def write(self, frames: object) -> int:
                self.events.append("write")
                self.frames = 4
                return self.frames

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats(frames_processed=self.frames)

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = OutputHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> OutputHandle:
                return self.handle

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        backend = Backend()
        ta.set_backend(backend)
        stats = ta.play(b"\x00" * 16, channels=1, timeout=0.1)
        self.assertEqual(stats.frames_processed, 4)
        self.assertEqual(backend.handle.events, ["start", "write", "drain"])

    def test_output_stream_write_all_handles_partial_writes(self) -> None:
        class OutputHandle:
            def __init__(self) -> None:
                self.frames = 0

            def start(self) -> None:
                pass

            def stop(self) -> None:
                pass

            def drain(self, timeout: float | None = None) -> bool:
                return True

            def flush(self) -> None:
                pass

            def close(self) -> None:
                pass

            def write(self, frames: object) -> int:
                del frames
                self.frames += 1
                return 1

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats(frames_processed=self.frames)

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = OutputHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> OutputHandle:
                return self.handle

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        ta.set_backend(Backend())
        stream = ta.OutputStream(channels=1)
        try:
            self.assertEqual(stream.write_all(b"\x00" * 12, timeout=0.1), 3)
            self.assertEqual(stream.stats().frames_processed, 3)
        finally:
            stream.close()

    def test_output_stream_write_all_times_out(self) -> None:
        class OutputHandle:
            def start(self) -> None:
                pass

            def stop(self) -> None:
                pass

            def drain(self, timeout: float | None = None) -> bool:
                return True

            def flush(self) -> None:
                pass

            def close(self) -> None:
                pass

            def write(self, frames: object) -> int:
                return 0

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats()

        class Backend:
            name = "test"

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> OutputHandle:
                return OutputHandle()

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

        ta.set_backend(Backend())
        stream = ta.OutputStream(channels=1)
        try:
            with self.assertRaises(TimeoutError):
                stream.write_all(b"\x00" * 4, timeout=0.001)
        finally:
            stream.close()

    def test_input_stream_lifecycle_delegates_to_backend(self) -> None:
        class InputHandle:
            def __init__(self) -> None:
                self.started = False
                self.closed = False
                self.queued_frames = 4

            def start(self) -> None:
                self.started = True

            def stop(self) -> None:
                self.started = False

            def close(self) -> None:
                self.closed = True

            def flush(self) -> None:
                self.queued_frames = 0

            def read(self, frame_count: int) -> memoryview:
                self.queued_frames = 0
                return memoryview(b"\x00" * frame_count * 4)

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats(frames_processed=4, queued_frames=self.queued_frames)

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = InputHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> InputHandle:
                return self.handle

        backend = Backend()
        ta.set_backend(backend)

        stream = ta.InputStream(channels=1)
        stream.start()
        self.assertTrue(backend.handle.started)
        self.assertEqual(len(stream.read(4)), 16)
        self.assertEqual(stream.stats().queued_frames, 0)
        stream.flush()
        stream.stop()
        self.assertFalse(backend.handle.started)
        stream.close()
        self.assertTrue(stream.closed)
        self.assertTrue(backend.handle.closed)

        with self.assertRaises(ta.StreamClosed):
            stream.read(1)

    def test_input_stream_read_exactly_combines_chunks(self) -> None:
        class InputHandle:
            def __init__(self) -> None:
                self.chunks = [b"\x01" * 8, b"\x02" * 8]

            def start(self) -> None:
                pass

            def stop(self) -> None:
                pass

            def close(self) -> None:
                pass

            def flush(self) -> None:
                pass

            def read(self, frame_count: int) -> memoryview:
                del frame_count
                return memoryview(self.chunks.pop(0) if self.chunks else b"")

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats()

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = InputHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> InputHandle:
                return self.handle

        ta.set_backend(Backend())
        stream = ta.InputStream(channels=1)
        try:
            data = stream.read_exactly(4, timeout=0.1)
            self.assertEqual(bytes(data), b"\x01" * 8 + b"\x02" * 8)
        finally:
            stream.close()

    def test_input_stream_read_exactly_times_out(self) -> None:
        class InputHandle:
            def start(self) -> None:
                pass

            def stop(self) -> None:
                pass

            def close(self) -> None:
                pass

            def flush(self) -> None:
                pass

            def read(self, frame_count: int) -> memoryview:
                return memoryview(b"")

            def stats(self) -> ta.StreamStats:
                return ta.StreamStats()

        class Backend:
            name = "test"

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> InputHandle:
                return InputHandle()

        ta.set_backend(Backend())
        stream = ta.InputStream(channels=1)
        try:
            with self.assertRaises(TimeoutError):
                stream.read_exactly(1, timeout=0.001)
        finally:
            stream.close()

    def test_duplex_stream_lifecycle_delegates_to_backend(self) -> None:
        class DuplexHandle:
            def __init__(self) -> None:
                self.started = False
                self.closed = False
                self.flushed = False
                self.written = 0

            def start(self) -> None:
                self.started = True

            def stop(self) -> None:
                self.started = False

            def flush(self) -> None:
                self.flushed = True

            def close(self) -> None:
                self.closed = True

            def write(self, frames: object) -> int:
                del frames
                self.written += 1
                return 1

            def read(self, frame_count: int) -> memoryview:
                return memoryview(b"\x00" * frame_count * 4)

            def stats(self) -> ta.DuplexStreamStats:
                return ta.DuplexStreamStats(
                    input=ta.StreamStats(frames_processed=4),
                    output=ta.StreamStats(frames_processed=self.written),
                )

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = DuplexHandle()
                self.config: object | None = None

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_duplex_stream(self, config: object) -> DuplexHandle:
                self.config = config
                return self.handle

        backend = Backend()
        ta.set_backend(backend)

        stream = ta.DuplexStream(input_channels=1, output_channels=1)
        self.assertEqual(stream.config.input_channels, 1)
        self.assertEqual(stream.config.output_channels, 1)
        self.assertIs(backend.config, stream.config)
        stream.start()
        self.assertTrue(backend.handle.started)
        self.assertEqual(stream.write(b"\x00" * 4), 1)
        self.assertEqual(len(stream.read(2)), 8)
        stats = stream.stats()
        self.assertEqual(stats.input.frames_processed, 4)
        self.assertEqual(stats.output.frames_processed, 1)
        stream.flush()
        self.assertTrue(backend.handle.flushed)
        stream.stop()
        self.assertFalse(backend.handle.started)
        stream.close()
        self.assertTrue(stream.closed)
        self.assertTrue(backend.handle.closed)

        with self.assertRaises(ta.StreamClosed):
            stream.read(1)

    def test_duplex_stream_blocking_helpers(self) -> None:
        class DuplexHandle:
            def __init__(self) -> None:
                self.output_frames = 0
                self.input_chunks = [b"\x01" * 8, b"\x02" * 8]

            def start(self) -> None:
                pass

            def stop(self) -> None:
                pass

            def flush(self) -> None:
                pass

            def close(self) -> None:
                pass

            def write(self, frames: object) -> int:
                del frames
                self.output_frames += 1
                return 1

            def read(self, frame_count: int) -> memoryview:
                del frame_count
                return memoryview(self.input_chunks.pop(0) if self.input_chunks else b"")

            def stats(self) -> ta.DuplexStreamStats:
                return ta.DuplexStreamStats(input=ta.StreamStats(), output=ta.StreamStats())

        class Backend:
            name = "test"

            def __init__(self) -> None:
                self.handle = DuplexHandle()

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_input_stream(self, config: object) -> object:
                raise NotImplementedError

            def open_duplex_stream(self, config: object) -> DuplexHandle:
                return self.handle

        ta.set_backend(Backend())
        stream = ta.DuplexStream(input_channels=1, output_channels=1)
        try:
            self.assertEqual(stream.write_all(b"\x00" * 12, timeout=0.1), 3)
            data = stream.read_exactly(4, timeout=0.1)
            self.assertEqual(bytes(data), b"\x01" * 8 + b"\x02" * 8)
        finally:
            stream.close()

    def test_duplex_stream_uses_backend_level_open(self) -> None:
        class Backend:
            name = "test"

            def list_devices(self) -> tuple[ta.DeviceInfo, ...]:
                return ()

            def open_output_stream(self, config: object) -> object:
                raise AssertionError("duplex must not compose output streams")

            def open_input_stream(self, config: object) -> object:
                raise AssertionError("duplex must not compose input streams")

            def open_duplex_stream(self, config: object) -> object:
                raise ta.BackendUnavailable("duplex unavailable")

        ta.set_backend(Backend())
        with self.assertRaises(ta.BackendUnavailable):
            ta.DuplexStream()

    def test_stream_stats_preserves_zero_latency(self) -> None:
        stats = ta.StreamStats(estimated_latency=0.0)
        self.assertEqual(stats.estimated_latency, 0.0)

    def test_stream_stats_validates_richer_fields(self) -> None:
        stats = ta.StreamStats(
            hardware_latency=0.01,
            queued_frames=12,
            queued_latency=0.25,
            buffer_size=256,
        )
        self.assertEqual(stats.hardware_latency, 0.01)
        self.assertEqual(stats.queued_frames, 12)
        self.assertEqual(stats.queued_latency, 0.25)
        self.assertEqual(stats.buffer_size, 256)

        with self.assertRaises(ValueError):
            ta.StreamStats(hardware_latency=-0.1)
        with self.assertRaises(ValueError):
            ta.StreamStats(queued_frames=-1)
        with self.assertRaises(ValueError):
            ta.StreamStats(queued_latency=-0.1)
        with self.assertRaises(ValueError):
            ta.StreamStats(buffer_size=0)


if __name__ == "__main__":
    unittest.main()
