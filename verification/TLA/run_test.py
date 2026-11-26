"""Check evidence isolation and fail when a named implementation test is absent."""

from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent
RUN = ROOT / "run"


class RunnerTest(unittest.TestCase):
    def test_parallel_invocations_keep_distinct_evidence(self):
        fixture = ROOT / "matching/reduce.json"

        def replay():
            result = subprocess.run(
                [RUN, "replay", "matching", fixture], capture_output=True, text=True,
                timeout=60, check=True,
            )
            prefix = result.stdout.splitlines()[0]
            self.assertTrue(prefix.startswith("Evidence: "))
            evidence = Path(prefix.removeprefix("Evidence: "))
            self.assertTrue((evidence / "matching-replay").is_file())
            return evidence

        with ThreadPoolExecutor(max_workers=2) as pool:
            first, second = (job.result() for job in (pool.submit(replay), pool.submit(replay)))
        self.assertNotEqual(first, second)

    def test_missing_one_mapped_test_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "CTestTestfile.cmake").write_text(
                'add_test(test_replication "/bin/true")\n'
            )
            command = [RUN, "check", "recovery"]
            for arguments, environment in (
                (["--build", directory], os.environ),
                ([], {**os.environ, "MEX_VERIFY_BUILD": directory}),
            ):
                with self.subTest(arguments=arguments):
                    result = subprocess.run(
                        [*command, *arguments], env=environment, capture_output=True,
                        text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("Missing CTest entries: replicated_smoke", result.stderr)


if __name__ == "__main__":
    unittest.main()
