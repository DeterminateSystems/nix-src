"""Regression checks using the local server started by gcs-binary-cache.sh."""

import json
import pathlib
import signal
import subprocess
import sys
import time
import unittest


server_dir = pathlib.Path(sys.argv[1])
config_nix = sys.argv[2]


def run(*args, check=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if check and result.returncode:
        raise AssertionError(f"{args} failed ({result.returncode}):\n{result.stderr}")
    return result


class GCSRegressions(unittest.TestCase):
    def test_upload_checksum_failure(self):
        payload = server_dir / "bad-upload-payload"
        payload.write_text("an upload whose response has the wrong checksum")
        store_path = run("nix-store", "--add", str(payload)).stdout.strip()
        result = run("nix", "copy", "--to", "gs://test/bad-upload?compression=none", store_path, check=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("checksum mismatch uploading", result.stderr)
        # Failed NAR uploads must not publish a narinfo advertising that object.
        requests = [json.loads(line) for line in (server_dir / "requests").read_text().splitlines()]
        names = [r["name"] for r in requests if r["method"] == "POST" and r["name"]]
        self.assertTrue(any(name.startswith("bad-upload/nar/") for name in names))
        self.assertFalse(any(name.startswith("bad-upload/") and name.endswith(".narinfo") for name in names))

    def test_interrupt_stops_retries(self):
        marker = server_dir / "interrupted"
        with subprocess.Popen(
            ["nix", "store", "info", "--store", "gs://interrupted/cache"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        ) as process:
            try:
                deadline = time.monotonic() + 5
                while not marker.exists():
                    self.assertIsNone(process.poll(), "Nix exited before the retry test started")
                    self.assertLess(time.monotonic(), deadline, "Nix never contacted the test server")
                    time.sleep(0.01)
                process.send_signal(signal.SIGINT)
                _, stderr = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0, stderr)
                self.assertIn("interrupted", stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_build_log_with_query_characters(self):
        # '?' and '=' are valid in store path names, but have meaning in URLs.
        expression = (
            f"with import {config_nix}; mkDerivation {{ "
            'name = "gcs?log=test"; buildCommand = "echo gcs-build-log; echo done > $out"; }'
        )
        output = run("nix-build", "--no-out-link", "--expr", expression).stdout.strip()
        derivation = run("nix-store", "--query", "--deriver", output).stdout.strip()
        run("nix", "store", "copy-log", "--to", "gs://test/logs", derivation)
        result = run("nix", "log", "--store", "gs://test/logs", derivation)
        self.assertIn("gcs-build-log", result.stdout)
        requests = [json.loads(line) for line in (server_dir / "requests").read_text().splitlines()]
        self.assertTrue(any(r["name"] == "logs/log/" + pathlib.Path(derivation).name for r in requests))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
