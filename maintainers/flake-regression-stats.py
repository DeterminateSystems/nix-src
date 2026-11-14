#!/usr/bin/env nix
#!nix shell --inputs-from . nixpkgs#python3 --command python3

# Collect and aggregate the resource usage statistics of flake-regressions runs.
#
# `collect` runs in each flake_regressions matrix job and gathers the
# `eval.timings` files written by flake-regressions' `eval-flake.sh` into a
# single JSON file. `aggregate` runs in a job that depends on all matrix jobs,
# and sums the statistics per Nix configuration.

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

FIELDS = ["elapsed", "user", "kernel", "mem"]


def parse_timings(path: Path) -> dict[str, float] | None:
    # The timings file is written by `time -f 'elapsed=%e user=%U kernel=%S mem=%M'`.
    # On failure, `time` prepends a "Command exited with non-zero status" line.
    lines = path.read_text().splitlines()
    if not lines:
        return None
    stats = dict(kv.split("=", 1) for kv in lines[-1].split())
    try:
        return {field: float(stats[field]) for field in FIELDS}
    except (KeyError, ValueError):
        return None


def collect(args: argparse.Namespace) -> None:
    flakes = []
    for timings in sorted(args.tests_dir.glob("*/*/*/eval.timings")):
        flake_dir = timings.parent
        if (flake_dir / "failed").exists():
            continue
        stats = parse_timings(timings)
        if stats is None:
            print(f"warning: cannot parse {timings}", file=sys.stderr)
            continue
        flakes.append({"flake": str(flake_dir.relative_to(args.tests_dir)), **stats})

    result = {
        "nix_config": args.nix_config,
        "glob": args.glob,
        "retried": args.retried,
        "flakes": flakes,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Collected statistics for {len(flakes)} flakes into {args.output}.")


def aggregate(args: argparse.Namespace) -> None:
    groups: dict[str, dict] = defaultdict(
        lambda: {
            "totals": {
                "jobs": 0,
                "retried_jobs": 0,
                "flakes": 0,
                "max_mem": 0.0,
                **{field: 0.0 for field in FIELDS},
            },
            "jobs": [],
        }
    )

    for path in sorted(args.input_dir.glob("*.json")):
        job = json.loads(path.read_text())
        group = groups[job["nix_config"]]
        group["jobs"].append(job)
        totals = group["totals"]
        totals["jobs"] += 1
        totals["retried_jobs"] += job["retried"]
        for flake in job["flakes"]:
            totals["flakes"] += 1
            for field in FIELDS:
                totals[field] += flake[field]
            totals["max_mem"] = max(totals["max_mem"], flake["mem"])

    for group in groups.values():
        group["jobs"].sort(key=lambda job: job["glob"])

    if args.output:
        args.output.write_text(json.dumps(groups, indent=2, sort_keys=True) + "\n")

    # `mem` is the maximum resident set size in KiB.
    gib = 1024 * 1024
    lines = [
        "## Flake regression statistics",
        "",
        "| Configuration | Jobs | Flakes | Elapsed (s) | User (s) | Kernel (s) | Σ max RSS (GiB) | Max RSS (GiB) |",
        "|---|--:|--:|--:|--:|--:|--:|--:|",
    ]
    for nix_config, group in sorted(groups.items()):
        t = group["totals"]
        jobs = str(t["jobs"])
        if t["retried_jobs"]:
            jobs += f" ({t['retried_jobs']} retried)"
        lines.append(
            f"| `{nix_config}` | {jobs} | {t['flakes']} "
            f"| {t['elapsed']:.1f} | {t['user']:.1f} | {t['kernel']:.1f} "
            f"| {t['mem'] / gib:.1f} | {t['max_mem'] / gib:.2f} |"
        )
    if any(group["totals"]["retried_jobs"] for group in groups.values()):
        lines += [
            "",
            "Retried jobs re-evaluate all their flakes against an empty store, so their timings are not comparable.",
        ]

    for nix_config, group in sorted(groups.items()):
        flakes = [flake for job in group["jobs"] for flake in job["flakes"]]
        slowest = sorted(flakes, key=lambda flake: flake["elapsed"], reverse=True)[: args.top]
        lines += [
            "",
            f"### Slowest flakes for `{nix_config}`",
            "",
            "| Flake | Elapsed (s) | User (s) | Kernel (s) | Max RSS (GiB) |",
            "|---|--:|--:|--:|--:|",
        ]
        for flake in slowest:
            lines.append(
                f"| `{flake['flake']}` | {flake['elapsed']:.1f} | {flake['user']:.1f} "
                f"| {flake['kernel']:.1f} | {flake['mem'] / gib:.2f} |"
            )

    summary = "\n".join(lines) + "\n"
    print(summary)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(summary)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(required=True)

    p = subparsers.add_parser("collect", help="collect the statistics of one flake_regressions job")
    p.add_argument("--tests-dir", type=Path, required=True)
    p.add_argument("--nix-config", required=True)
    p.add_argument("--glob", required=True)
    p.add_argument("--retried", action="store_true")
    p.add_argument("--output", type=Path, required=True)
    p.set_defaults(func=collect)

    p = subparsers.add_parser("aggregate", help="aggregate the statistics of all flake_regressions jobs")
    p.add_argument("--input-dir", type=Path, required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--summary", type=Path, help="append a Markdown summary to this file")
    p.add_argument("--top", type=int, default=10, help="number of slowest flakes to show per configuration")
    p.set_defaults(func=aggregate)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
