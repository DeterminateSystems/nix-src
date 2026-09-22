#!/usr/bin/env python3
"""Run the functional tests without Meson.

This provides what `meson test` provides to the test scripts: the generated
`common/subst-vars.sh` and `config.nix`, the environment variables the
harness in `common/` expects, and a per-test `TEST_ROOT` (derived by the
harness from `TEST_SUITE_NAME` and `TEST_NAME`) so that tests can run in
parallel. Tests are not modified; each is run as `bash -x -e -u -o pipefail
<script>` from this directory, like Meson does.

Usable for development (`run-tests --nix-bin-dir ... simple.sh` from the
devShell of packaging/nix-make, see test-runner.nix) and inside a Nix
derivation (see make.nix).
"""

import argparse
import concurrent.futures
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SKIP_EXIT_CODE = 77  # As in `skipTest` in common/functions.sh (and Meson).


def which_or_die(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f"error: '{name}' not found in PATH")
    return path


def configure(source_dir, build_dir, nix_bin_dir):
    """Generate the files that Meson's `configure_file()` would."""
    nix = nix_bin_dir / "nix"
    if not nix.exists():
        sys.exit(f"error: '{nix}' does not exist")
    system = subprocess.run(
        [nix, "config", "show", "system"], check=True, capture_output=True, text=True
    ).stdout.strip()
    values = {
        "bash": which_or_die("bash"),
        "bindir": str(nix_bin_dir),
        "coreutils": str(Path(which_or_die("ls")).parent),
        "dot": shutil.which("dot") or "",
        "sandbox_shell": shutil.which("busybox") or "",
        "PACKAGE_VERSION": (source_dir / ".version").read_text().strip(),
        "system": system,
    }
    for template, output in [
        ("common/subst-vars.sh.in", "common/subst-vars.sh"),
        ("config.nix.in", "config.nix"),
    ]:
        text = (source_dir / template).read_text()
        for name, value in values.items():
            text = text.replace(f"@{name}@", value)
        out = build_dir / output
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)


class Runner:
    def __init__(self, args, source_dir, build_dir):
        self.args = args
        self.source_dir = source_dir
        self.build_dir = build_dir
        self.log_dir = build_dir / "logs"
        self.log_dir.mkdir(parents=True, exist_ok=True)
        # The running children, so that they can be killed on interruption.
        self.children = set()
        self.lock = threading.Lock()
        self.interrupted = False

    def env_for(self, name):
        env = dict(os.environ)
        # The harness tests `[[ -n $NIX_STORE ]]` under `set -u` to detect
        # running inside a Nix build (where nested builds cannot sandbox).
        env.setdefault("NIX_STORE", "")
        env.update(
            {
                "_NIX_TEST_SOURCE_DIR": str(self.source_dir),
                "_NIX_TEST_BUILD_DIR": str(self.build_dir),
                "TEST_SUITE_NAME": self.args.suite,
                "TEST_NAME": name,
                "NIX_REMOTE": "",
                "PS4": "+(${BASH_SOURCE[0]-$0}:$LINENO) ",
                "ASAN_OPTIONS": "abort_on_error=1:print_summary=1:detect_leaks=0",
            }
        )
        return env

    @staticmethod
    def kill_group(child):
        """Terminate a test and everything it spawned (it runs in its own
        session, so they form one process group)."""
        for sig, grace in [(signal.SIGTERM, 5), (signal.SIGKILL, None)]:
            try:
                os.killpg(os.getpgid(child.pid), sig)
            except ProcessLookupError:
                return
            try:
                child.wait(timeout=grace)
                return
            except subprocess.TimeoutExpired:
                continue

    def run_test(self, script):
        name = script[: -len(".sh")] if script.endswith(".sh") else script
        log_path = self.log_dir / (name.replace("/", "-") + ".log")
        start = time.monotonic()
        with open(log_path, "wb") as log:
            child = subprocess.Popen(
                ["bash", "-x", "-e", "-u", "-o", "pipefail", script],
                cwd=self.source_dir,
                env=self.env_for(name),
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            with self.lock:
                if self.interrupted:
                    self.kill_group(child)
                    return name, "INTERRUPTED", 0, log_path
                self.children.add(child)
            try:
                child.wait(timeout=self.args.timeout)
                status = {0: "OK", SKIP_EXIT_CODE: "SKIP"}.get(child.returncode, "FAIL")
            except subprocess.TimeoutExpired:
                self.kill_group(child)
                status = "TIMEOUT"
            finally:
                with self.lock:
                    self.children.discard(child)
        if self.interrupted:
            status = "INTERRUPTED"
        return name, status, time.monotonic() - start, log_path

    def interrupt(self):
        with self.lock:
            self.interrupted = True
            children = list(self.children)
        for child in children:
            self.kill_group(child)

    def run(self, scripts):
        results = []
        width = max(len(s) for s in scripts)
        executor = concurrent.futures.ThreadPoolExecutor(self.args.jobs)
        futures = [executor.submit(self.run_test, s) for s in scripts]
        try:
            for future in concurrent.futures.as_completed(futures):
                name, status, seconds, log_path = future.result()
                results.append((name, status, log_path))
                print(f"{status:<11} {name:<{width}} {seconds:6.2f}s", flush=True)
        except KeyboardInterrupt:
            print("\ninterrupted, terminating running tests...", flush=True)
            executor.shutdown(wait=False, cancel_futures=True)
            self.interrupt()
            executor.shutdown(wait=True)
            return 130
        executor.shutdown()
        return self.report(results)

    def report(self, results):
        counts = {}
        for _, status, _ in results:
            counts[status] = counts.get(status, 0) + 1
        print()
        print("  ".join(f"{s}: {counts.get(s, 0)}" for s in ["OK", "FAIL", "SKIP", "TIMEOUT"]))
        failed = [(n, s, l) for n, s, l in results if s in ("FAIL", "TIMEOUT")]
        if failed and not self.args.quiet:
            for name, status, log_path in failed:
                print(f"\n==== {status}: {name} ({log_path}) ====")
                sys.stdout.write(log_path.read_text(errors="replace"))
        return 1 if failed else 0


def default_source_dir():
    # When installed (see test-runner.nix), the script does not live in the
    # tests directory, so fall back to the current directory.
    here = Path(__file__).resolve().parent
    return here if (here / "common" / "init.sh").exists() else Path.cwd()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("tests", nargs="+", help="test scripts, relative to the source dir")
    parser.add_argument("--nix-bin-dir", type=Path, help="directory of the nix to test (default: from PATH)")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=default_source_dir(),
        help="the tests/functional directory (default: this script's directory if it is one, else the current directory)",
    )
    parser.add_argument("--build-dir", type=Path, help="where to put generated files and logs (default: a temporary dir)")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--timeout", type=float, default=300, help="per-test timeout in seconds")
    parser.add_argument("--suite", default="main", help="value of TEST_SUITE_NAME")
    parser.add_argument("--quiet", "-q", action="store_true", help="do not print the logs of failed tests")
    args = parser.parse_args()

    source_dir = args.source_dir.resolve()
    nix_bin_dir = (args.nix_bin_dir or Path(which_or_die("nix")).parent).resolve()
    build_dir = (args.build_dir or Path(tempfile.mkdtemp(prefix="nix-functional-tests-"))).resolve()

    configure(source_dir, build_dir, nix_bin_dir)
    print(f"testing {nix_bin_dir}/nix; logs in {build_dir}/logs", flush=True)

    # Make SIGTERM behave like Ctrl-C, so that children are cleaned up either way.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    sys.exit(Runner(args, source_dir, build_dir).run(args.tests))


if __name__ == "__main__":
    main()
