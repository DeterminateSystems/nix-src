#!/usr/bin/env bash

source common.sh

# This doesn't actually work, but it prevents sentry from uploading for real.
export NIX_SENTRY_ENDPOINT=file://$TEST_ROOT/sentry-endpoint

ulimit -c 0

sentryDir="$TEST_HOME/.cache/nix/sentry"

nix --version
if ! [[ -d $sentryDir ]]; then
    skipTest "not built with sentry support"
fi

for type in segfault assert logic-error; do
    rm -rf "$sentryDir"

    (! nix __crash "$type")

    [[ -e $sentryDir/last_crash ]]

    envelopes=("$sentryDir"/pending/*.dmp)
    if [[ ! -e "${envelopes[0]}" ]]; then
        fail "No crash dump found in $sentryDir after crash"
    fi
done
