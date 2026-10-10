#!/usr/bin/env nix
#!nix shell --inputs-from .. nixpkgs#python3 --command python3

"""
Benchmark Nix evaluation performance across a set of Nix releases.

For each tag (e.g. `v3.23.1` for Determinate Nix, `2.35.2` for upstream Nix,
or a full Git revision of DeterminateSystems/nix-src), each test and each run
number, this runs the test and appends a row to the CSV file. Runs that are
already recorded in the CSV file are skipped, and Nix versions are only built
when needed.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from pathlib import Path

NIXPKGS = "github:NixOS/nixpkgs/531670d871c0e29724a02f3cbcac170adc65b58c"

TESTS = {
    "search": ["search", NIXPKGS, "fizzbuzz", "--no-eval-cache"],
    "firefox": ["eval", "--json", f"{NIXPKGS}#firefox", "--read-only"],
    "plasma": [
        "eval",
        "--json",
        "-I",
        f"nixpkgs=flake:{NIXPKGS}",
        "--file",
        "<nixpkgs/nixos/release-combined.nix>",
        "nixos.tests.plasma6.x86_64-linux",
        "--read-only",
    ],
}

EXTRA_ARGS = ["--option", "eval-cores", "0"]

HEADER = ["nix-tag", "test-name", "test-run", "elapsed-time", "cpu-time", "kernel-time", "max-rss-kib"]


def flake_ref(tag: str) -> str:
    # Determinate Nix tags and revisions come from nix-src, upstream tags from NixOS/nix.
    if tag.startswith("v") or re.fullmatch(r"[0-9a-f]{40}", tag):
        return f"github:DeterminateSystems/nix-src/{tag}"
    return f"github:NixOS/nix/{tag}"


def build_nix(tag: str) -> Path | None:
    ref = flake_ref(tag)
    print(f"building {ref}...", file=sys.stderr)
    res = subprocess.run(["nix", "build", "--no-link", "--print-out-paths", ref], stdout=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"failed to build {ref}, skipping", file=sys.stderr)
        return None
    # The package has multiple outputs (e.g. `man`), so find the one with the binary.
    for out in res.stdout.split():
        nix_bin = Path(out) / "bin" / "nix"
        if os.access(nix_bin, os.X_OK):
            return nix_bin
    print(f"{ref} does not provide bin/nix, skipping", file=sys.stderr)
    return None


def run_test(nix_bin: Path, test: str) -> list[str] | None:
    """Run a test, returning the measurements, or None if the test failed."""
    start = time.monotonic()
    proc = subprocess.Popen([nix_bin, *TESTS[test], *EXTRA_ARGS], stdout=subprocess.DEVNULL)
    _, status, rusage = os.wait4(proc.pid, 0)
    elapsed = time.monotonic() - start
    proc.returncode = os.waitstatus_to_exitcode(status)
    if proc.returncode != 0:
        return None
    return [
        f"{elapsed:.3f}",
        f"{rusage.ru_utime:.3f}",
        f"{rusage.ru_stime:.3f}",
        str(rusage.ru_maxrss),  # KiB on Linux
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tags", metavar="TAG", nargs="+", help="Nix release tag or nix-src revision")
    parser.add_argument("-o", "--output", type=Path, default=Path("eval-benchmark.csv"), help="CSV file to append to")
    parser.add_argument("-n", "--runs", type=int, default=5, help="number of runs per test/tag pair")
    parser.add_argument("-t", "--test", dest="tests", action="append", choices=TESTS.keys(), help="test to run (default: all)")
    args = parser.parse_args()

    tests = args.tests or list(TESTS.keys())

    done = set()
    if args.output.exists():
        with args.output.open(newline="") as f:
            for row in csv.DictReader(f):
                done.add((row["nix-tag"], row["test-name"], row["test-run"]))
    else:
        with args.output.open("w", newline="") as f:
            csv.writer(f).writerow(HEADER)

    for tag in args.tags:
        nix_bin = None
        build_failed = False

        for test in tests:
            if build_failed:
                break

            for run in range(1, args.runs + 1):
                if (tag, test, str(run)) in done:
                    continue

                if nix_bin is None:
                    nix_bin = build_nix(tag)
                    if nix_bin is None:
                        build_failed = True
                        break

                measurements = run_test(nix_bin, test)
                if measurements is None:
                    print(f"{tag} {test} {run}: failed", file=sys.stderr)
                    break

                # Append each row immediately so that an interrupted run loses at most one measurement.
                with args.output.open("a", newline="") as f:
                    csv.writer(f).writerow([tag, test, run, *measurements])
                print(f"{tag} {test} {run}: {measurements[0]} s", file=sys.stderr)


if __name__ == "__main__":
    main()
