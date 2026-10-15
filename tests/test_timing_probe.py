from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "measure_timing", Path(__file__).resolve().parents[1] / "examples/measure_timing.py"
)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
summarize = probe.summarize
trial_metrics = probe.trial_metrics


class TimingProbeTests(unittest.TestCase):
    def test_summary_keeps_latency_and_jitter_distinct(self) -> None:
        result = summarize([10.0, 10.0, 10.0])
        self.assertEqual(result["mean"], 10.0)
        self.assertEqual(result["stddev"], 0.0)
        self.assertEqual(result["p99"], 10.0)

    def test_percentiles_include_slow_trials(self) -> None:
        result = summarize([3.0, 1.0, 2.0, 100.0])
        self.assertEqual(result["median"], 2.5)
        self.assertAlmostEqual(result["p95"], 85.45)
        self.assertEqual(result["max"], 100.0)

    def test_metrics_separate_scheduler_queue_and_buffer_return(self) -> None:
        metrics = trial_metrics({
            "target_ns": 1_000_000_000,
            "write_begin_ns": 1_002_000_000,
            "write_end_ns": 1_002_010_000,
            "data_copy_ns": 1_008_000_000,
            "enqueue_begin_ns": 1_008_001_000,
            "enqueue_end_ns": 1_008_003_000,
            "data_complete_ns": 1_038_000_000,
        })
        self.assertEqual(metrics["scheduling_error_ms"], 2.0)
        self.assertEqual(metrics["write_duration_ms"], 0.01)
        self.assertEqual(metrics["write_to_copy_ms"], 6.0)
        self.assertEqual(metrics["target_to_enqueue_ms"], 8.003)
        self.assertEqual(metrics["enqueue_duration_ms"], 0.002)
        self.assertEqual(metrics["target_to_buffer_return_ms"], 38.0)
