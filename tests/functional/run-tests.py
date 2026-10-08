#!/usr/bin/env python3
"""Run the functional tests without Meson.

This provides what `meson test` provides to the test scripts: the generated
`common/subst-vars.sh` and `config.nix`, the environment variables the
harness in `common/` expects, and a per-test `TEST_ROOT` (derived by the
harness from `TEST_SUITE_NAME` and `TEST_NAME`) so that tests can run in
parallel. Tests are not modified; each is run as `bash -x -e -u -o pipefail
<script>` from its directory, like Meson does.

Usable for development (`run-tests --nix-bin-dir ... simple` from anywhere
in the repository, in the devShell of packaging/nix-make, see
test-runner.nix) and inside a Nix derivation (see make.nix).
"""

import argparse
import concurrent.futures
import fnmatch
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SKIP_EXIT_CODE = 77  # As in `skipTest` in common/functions.sh (and Meson).

# ANSI colors per status, used when writing to a terminal (and NO_COLOR is unset).
COLORS = {"OK": "\033[32m", "SKIP": "\033[33m", "FAIL": "\033[31m", "TIMEOUT": "\033[31m"}
RESET = "\033[0m"


def colored(status, text):
    if sys.stdout.isatty() and not os.environ.get("NO_COLOR") and status in COLORS:
        return f"{COLORS[status]}{text}{RESET}"
    return text


# Scripts that are sourced or executed by tests rather than being tests.
HELPER_PATTERNS = ["common.sh", "*-common.sh", "*-inner.sh"]

# Tests that `discover_tests` would find but that are not run by default,
# with the reason. They can still be run by naming them explicitly.
EXCLUDED_TESTS = {
    "parallel.sh": "unreliable; Meson does not run it either",
    "plugins.sh": "needs the test plugin built by Meson",
    "test-libstoreconsumer.sh": "needs the test program built by Meson",
}


def discover_tests(source_dir):
    """All tests under the source dir, relative to it.

    A test is a `*.sh` file that sources another script (ultimately the
    harness in common.sh) and whose name does not mark it as a helper. This
    reproduces the lists in the meson.build files without maintaining them.
    """
    tests = []
    for path in sorted(source_dir.rglob("*.sh")):
        rel = path.relative_to(source_dir)
        if rel.parts[0] == "common" or str(rel) in EXCLUDED_TESTS:
            continue
        if any(fnmatch.fnmatch(path.name, pattern) for pattern in HELPER_PATTERNS):
            continue
        if re.search(r"^\s*(source|\.)\s+\S+\.sh\b", path.read_text(errors="replace"), re.MULTILINE):
            tests.append(str(rel))
    return tests


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

    def env_for(self, suite, name):
        env = dict(os.environ)
        # The harness tests `[[ -n $NIX_STORE ]]` under `set -u` to detect
        # running inside a Nix build (where nested builds cannot sandbox).
        env.setdefault("NIX_STORE", "")
        # The stdenv setup script exports `shell`, which some tests use.
        env.setdefault("shell", which_or_die("bash"))
        env.update(
            {
                "_NIX_TEST_SOURCE_DIR": str(self.source_dir),
                "_NIX_TEST_BUILD_DIR": str(self.build_dir),
                "TEST_SUITE_NAME": suite,
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
        # A test in a subdirectory (e.g. `flakes/show.sh`) belongs to the
        # suite named after that directory and runs from it, like Meson does.
        path = Path(script)
        subdir = path.parent
        suite = self.args.suite if subdir == Path() else str(subdir)
        test = path.name[: -len(".sh")] if path.name.endswith(".sh") else path.name
        name = str(subdir / test)
        log_path = self.log_dir / (name.replace("/", "-") + ".log")
        start = time.monotonic()
        with open(log_path, "wb") as log:
            child = subprocess.Popen(
                ["bash", "-x", "-e", "-u", "-o", "pipefail", path.name],
                cwd=self.source_dir / subdir,
                env=self.env_for(suite, test),
                # No terminal on stdin (as with Meson): tests must not read
                # from it, and programs like an interactive bash would change
                # its settings, mangling the output.
                stdin=subprocess.DEVNULL,
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
                print(f"{colored(status, f'{status:<11}')} {name:<{width}} {seconds:6.2f}s", flush=True)
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
        print("  ".join(colored(s, f"{s}: {counts.get(s, 0)}") for s in ["OK", "FAIL", "SKIP", "TIMEOUT"]))
        failed = [(n, s, l) for n, s, l in results if s in ("FAIL", "TIMEOUT")]
        if failed and not self.args.quiet:
            for name, status, log_path in failed:
                print(colored(status, f"\n==== {status}: {name} ({log_path}) ===="))
                sys.stdout.write(log_path.read_text(errors="replace"))
        return 1 if failed else 0


def is_source_dir(path):
    return (path / "common" / "init.sh").exists()


def default_source_dir():
    """The tests/functional directory: the one containing this script, or
    else (when installed, see test-runner.nix) the one of the repository
    that the current directory is in."""
    here = Path(__file__).resolve().parent
    if is_source_dir(here):
        return here
    for dir in [Path.cwd(), *Path.cwd().parents]:
        if is_source_dir(dir / "tests" / "functional"):
            return dir / "tests" / "functional"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "tests", nargs="*", help="tests, relative to the source dir, e.g. 'simple' or 'flakes/show.sh' (default: all)"
    )
    parser.add_argument("--nix-bin-dir", type=Path, help="directory of the nix to test (default: from PATH)")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=default_source_dir(),
        help="the tests/functional directory (default: the one of the repository containing the current directory)",
    )
    parser.add_argument("--build-dir", type=Path, help="where to put generated files and logs (default: a temporary dir)")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--timeout", type=float, default=300, help="per-test timeout in seconds")
    parser.add_argument(
        "--suite", default="main", help="TEST_SUITE_NAME for tests in the top-level directory (default: main)"
    )
    parser.add_argument("--quiet", "-q", action="store_true", help="do not print the logs of failed tests")
    args = parser.parse_args()

    if not args.source_dir:
        sys.exit("error: not inside a Nix source tree; use --source-dir")
    source_dir = args.source_dir.resolve()
    # Allow tests to be named without the `.sh` suffix.
    tests = [t if t.endswith(".sh") else t + ".sh" for t in args.tests] or discover_tests(source_dir)
    nix_bin_dir = (args.nix_bin_dir or Path(which_or_die("nix")).parent).resolve()
    build_dir = (args.build_dir or Path(tempfile.mkdtemp(prefix="nix-functional-tests-"))).resolve()

    configure(source_dir, build_dir, nix_bin_dir)
    print(f"testing {nix_bin_dir}/nix; logs in {build_dir}/logs", flush=True)

    # Make SIGTERM behave like Ctrl-C, so that children are cleaned up either way.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    sys.exit(Runner(args, source_dir, build_dir).run(tests))


if __name__ == "__main__":
    main()
