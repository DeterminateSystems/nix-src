#!/usr/bin/env bash

source common.sh

# Test that the output of builders ends up in the build log when one
# client starts many builds at the same time. Starting a build blocks
# the worker for a while, and on macOS, the output of a builder that
# has exited is discarded if it isn't read from the pseudoterminal
# within 0.6 s.

n=120

mapfile -t drvs < <(nix-instantiate build-log-many.nix --arg n "$n")
(( ${#drvs[@]} == n ))

nix-build build-log-many.nix --arg n "$n" --no-out-link --max-jobs "$n"

missing=0
for drv in "${drvs[@]}"; do
    i=${drv%.drv}
    i=${i##*-}
    if [[ "$(nix log "$drv")" != "output of build $i" ]]; then
        echo "the log of '$drv' is incomplete" >&2
        missing=$((missing + 1))
    fi
done

if (( missing > 0 )); then
    fail "$missing of $n build logs are incomplete"
fi
