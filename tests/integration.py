"""Exercise real IOKit assertions; only terminate processes created by this test."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


def cli(*args):
    return subprocess.run([BINARY, *map(str, args)], capture_output=True, text=True, timeout=10)


def listing(all_types=False):
    result = cli("list", "--json", *(["--all"] if all_types else []))
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)["processes"]


class Integration(unittest.TestCase):
    def setUp(self):
        self.children = []

    def tearDown(self):
        for process in self.children:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)

    def blocker(self, flag):
        process = subprocess.Popen(["/usr/bin/caffeinate", flag, "-t", "30"])
        self.children.append(process)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            entries = [p for p in listing(True) if p["pid"] == process.pid]
            if entries:
                return process, entries[0]
            time.sleep(0.05)
        self.fail("Test assertion did not become visible")

    def test_display_dry_run_and_sigterm(self):
        process, entry = self.blocker("-d")
        self.assertTrue(entry["can_terminate"])
        self.assertIn("PreventUserIdleDisplaySleep", [a["type"] for a in entry["assertions"]])
        self.assertIn(process.pid, [p["pid"] for p in listing()])
        preview = cli("kill", process.pid, "--force", "--dry-run")
        self.assertEqual(preview.returncode, 0, preview.stderr)
        self.assertIn("[dry-run]", preview.stdout)
        self.assertIn("SIGKILL", preview.stdout)
        self.assertIsNone(process.poll())
        result = cli("kill", process.pid)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(process.wait(timeout=5), -signal.SIGTERM)

    def test_system_scope_and_force_deduplicates_pid(self):
        process, entry = self.blocker("-i")
        self.assertIn("PreventUserIdleSystemSleep", [a["type"] for a in entry["assertions"]])
        self.assertNotIn(process.pid, [p["pid"] for p in listing()])
        result = cli("kill", process.pid, process.pid, "--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("SIGKILL"), 1)
        self.assertEqual(process.wait(timeout=5), -signal.SIGKILL)

    def test_activity_is_in_display_scope(self):
        process, entry = self.blocker("-u")
        self.assertIn("UserIsActive", [a["type"] for a in entry["assertions"]])
        self.assertIn(process.pid, [p["pid"] for p in listing()])

    def test_validate_entire_batch_before_signaling(self):
        process, _ = self.blocker("-d")
        result = cli("kill", process.pid, 2147483647, "--force")
        self.assertEqual(result.returncode, 1)
        self.assertIsNone(process.poll())

    def test_unrelated_process_is_not_terminated(self):
        process = subprocess.Popen(["/bin/sleep", "30"])
        self.children.append(process)
        result = cli("kill", process.pid, "--force")
        self.assertEqual(result.returncode, 1)
        self.assertIsNone(process.poll())

    def test_invalid_inputs(self):
        for args in [("kill",), ("kill", "0"), ("kill", "-1"), ("kill", "1"),
                     ("kill", "12x"), ("kill", "99999999999999999999999"),
                     ("kill", "--json"), ("list", "--force"), ("wat",),
                     ("kill", "--all"), ("list", "123")]:
            with self.subTest(args=args):
                self.assertEqual(cli(*args).returncode, 2)

    def test_help_and_version(self):
        self.assertIn("SIGKILL", cli("--help").stdout)
        self.assertEqual(cli("--version").stdout.strip(), "1.0.0")


if __name__ == "__main__":
    if os.getuid() == 0:
        sys.exit("Run tests as a regular user; root processes are protected.")
    unittest.main(verbosity=2)
