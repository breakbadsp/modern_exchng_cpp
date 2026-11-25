import argparse
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from report import classify, limits, rate_limits
from run import Case, record_case, validate


class RunnerTest(unittest.TestCase):
    def test_reconnect_cohort_must_fit_the_idle_sessions(self):
        args = argparse.Namespace(repeat=1, seconds=1, target_ms=100, orders=100,
                                  build=Path("/unused"))
        parser = argparse.ArgumentParser()
        with patch("run.os.sched_getaffinity", return_value=set(range(6))):
            with patch.object(parser, "error", side_effect=ValueError) as reject:
                with self.assertRaises(ValueError):
                    validate(args, [Case("reconnect", 64, 400_000, 512)], parser)
                reject.assert_called_once()
                self.assertIn("Invalid benchmark setting", reject.call_args.args[0])

    def test_worker_timeout_is_a_case_outcome(self):
        case = Case("active", 64, 100_000)
        with patch("run.run_case", side_effect=subprocess.TimeoutExpired("exchange", 15)):
            row = record_case(None, None, case, 1)
        self.assertEqual(row["status"], "invalid")
        self.assertIn("timed out", row["error"])


class LimitTest(unittest.TestCase):
    def test_driver_shortfall_cannot_count_as_a_pass(self):
        row = {"scenario": "active", "status": "complete", "offered_ops_s": 400_000,
               "ops_s": 380_000, "order_p100_ms": 10, "sender_p100_ms": 80}
        self.assertEqual(classify(row, 100), "driver-limited")

    def test_at_threshold_is_a_breach(self):
        row = {"scenario": "active", "status": "complete", "offered_ops_s": 400_000,
               "ops_s": 399_000, "order_p100_ms": 100, "sender_p100_ms": 2}
        self.assertEqual(classify(row, 100), "order P100")

    def test_one_failing_repeat_excludes_the_setting(self):
        seed = {"scenario": "active", "sessions": 256, "base": 400_000, "burst": 100_000,
                "offered_ops_s": 420_000, "status": "complete", "ops_s": 419_000,
                "order_p100_ms": 45, "sender_p100_ms": 7}
        rows = [seed, {**seed, "sessions": 1024}, {**seed, "sessions": 1024, "order_p100_ms": 102}]
        self.assertEqual(limits(rows, 100)["active"][0]["max_tested_passing_sessions"], 256)

    def test_rate_limit_ignores_undelivered_high_rate(self):
        row = {"scenario": "active", "sessions": 1024, "base": 100000, "burst": 0,
               "status": "complete", "offered_ops_s": 100000, "ops_s": 99999,
               "order_p100_ms": 11}
        overloaded = {**row, "base": 200000, "offered_ops_s": 200000, "ops_s": 150000}
        self.assertEqual(rate_limits([row, overloaded], 100)[0]["max_tested_passing_base_ops_s"],
                         100000)

    def test_reconnect_has_its_own_failure_reason(self):
        row = {"scenario": "reconnect", "status": "complete", "offered_ops_s": 400_000,
               "ops_s": 397_000, "order_p100_ms": 40, "logon_p100_ms": 105}
        self.assertEqual(classify(row, 100), "reconnect P100")


if __name__ == "__main__":
    unittest.main()
