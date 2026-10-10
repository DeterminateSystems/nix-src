#!/usr/bin/env nix
#! nix shell --impure --expr ``
#! nix with (builtins.getFlake (toString ./..)).inputs.nixpkgs.legacyPackages.${builtins.currentSystem};
#! nix python3.withPackages (ps: [ ps.matplotlib ])
#! nix ``
#! nix --command python3

"""Plot the minimum elapsed time and max RSS per Nix release for each test in an eval-benchmark.py CSV file."""

import argparse
import csv
import re
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("csv", nargs="?", default="eval-benchmark.csv", metavar="FILE.csv")
parser.add_argument(
    "-o", "--output", help="output file (default: eval-benchmark.png, or .svg with --svg)"
)
parser.add_argument(
    "--svg",
    action="store_true",
    help="write a transparent SVG styled for a dark background",
)
parser.add_argument("--log", action="store_true", help="use a logarithmic y-axis")
args = parser.parse_args()

if args.output is None:
    args.output = "eval-benchmark.svg" if args.svg else "eval-benchmark.png"

if args.svg:
    fg = "#dddddd"
    plt.rcParams.update({
        "svg.fonttype": "none",
        "text.color": fg,
        "axes.labelcolor": fg,
        "axes.edgecolor": fg,
        "axes.titlecolor": fg,
        "xtick.color": fg,
        "ytick.color": fg,
        "grid.color": fg,
        "legend.edgecolor": fg,
        "legend.facecolor": "none",
        "legend.labelcolor": fg,
        "figure.facecolor": "none",
        "axes.facecolor": "none",
        "savefig.facecolor": "none",
        "savefig.edgecolor": "none",
    })


def version_key(tag):
    """Sort key for tags such that v3.1.0 < v3.2.0 < v3.10.0."""
    return [int(p) if p.isdigit() else p for p in re.split(r"(\d+)", tag.removeprefix("v"))]


def is_revision(tag):
    return re.fullmatch(r"[0-9a-f]{40}", tag) is not None


def is_determinate(tag):
    # Revisions are of DeterminateSystems/nix-src.
    return tag.startswith("v") or is_revision(tag)


# (CSV column, y-axis label, scale factor), plotted as columns from left to right.
METRICS = [
    ("elapsed-time", "Elapsed time (s)", 1),
    ("max-rss-kib", "Max RSS (MiB)", 1 / 1024),
]

# data[metric][test][tag] = list of measurements
data = {metric: defaultdict(lambda: defaultdict(list)) for metric, _, _ in METRICS}
with open(args.csv, newline="") as f:
    for row in csv.DictReader(f):
        for metric, _, scale in METRICS:
            data[metric][row["test-name"]][row["nix-tag"]].append(float(row[metric]) * scale)

times = data["elapsed-time"]
tests = list(times)
# Release tags sorted by version, followed by revisions in the order in which they appear in the CSV file.
all_tags = list(dict.fromkeys(tag for per_tag in times.values() for tag in per_tag))
tags = sorted([t for t in all_tags if not is_revision(t)], key=version_key) + [t for t in all_tags if is_revision(t)]
x = {tag: i for i, tag in enumerate(tags)}

fig, axes = plt.subplots(
    len(tests),
    len(METRICS),
    figsize=(max(10, 0.4 * len(tags)) * len(METRICS), 4 * len(tests)),
    sharex=True,
    squeeze=False,
)

for row_axes, test in zip(axes, tests):
    for ax, (metric, ylabel, _) in zip(row_axes, METRICS):
        values = data[metric][test]
        for name, pred in [("Nix", lambda t: not is_determinate(t)), ("Determinate Nix", is_determinate)]:
            series = [tag for tag in tags if tag in values and pred(tag)]
            if series:
                ax.plot([x[t] for t in series], [min(values[t]) for t in series], marker=".", label=name)
        ax.set_title(test)
        ax.set_ylabel(ylabel)
        if args.log:
            ax.set_yscale("log")
        else:
            ax.set_ylim(bottom=0)
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
        ax.legend(title="minimum values")

for ax in axes[-1]:
    ax.set_xticks(range(len(tags)), [t[:10] if is_revision(t) else t for t in tags], rotation=45, ha="right")
    ax.set_xlabel("Nix release")
fig.suptitle("Nix evaluation performance")
fig.tight_layout()
if args.svg:
    fig.savefig(args.output, format="svg", transparent=True)
else:
    fig.savefig(args.output, dpi=150)
print(f"wrote {args.output}")
