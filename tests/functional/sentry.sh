#!/usr/bin/env bash

source common.sh

unset NIX_DISABLE_SENTRY

ulimit -c 0

sentryDir="$TEST_HOME/.cache/nix/sentry"

nix --version
if ! [[ -d $sentryDir ]]; then
    skip "not built with sentry support"
fi

for type in segfault assert logic-error; do
    rm -rf "$sentryDir"

    (! nix __crash "$type")

    [[ -e $sentryDir/last_crash ]]

    envelopes=("$sentryDir"/*.run/*.envelope)
    if [[ ! -e "${envelopes[0]}" ]]; then
        fail "No crash dump found in $sentryDir after crash"
    fi
done
