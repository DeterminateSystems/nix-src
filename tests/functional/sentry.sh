#!/usr/bin/env bash

source common.sh

# Enable sentry with a fake endpoint.
unset NIX_SENTRY_ENDPOINT
echo -n "file://$TEST_ROOT/sentry-endpoint" > "$NIX_CONF_DIR/sentry-endpoint"

ulimit -c 0

sentryDir="$TEST_HOME/.cache/nix/sentry"

nix --version
if ! [[ -d $sentryDir ]]; then
    skipTest "not built with sentry support"
fi

for type in segfault assert logic-error; do
    if [[ $type = logic-error && $(uname) = Darwin ]]; then continue; fi

    rm -rf "$sentryDir"

    (! nix __crash "$type")

    envelopes=("$sentryDir"/pending/*.dmp)
    if [[ ! -e "${envelopes[0]}" ]]; then
        fail "No crash dump found in $sentryDir after crash"
    fi
done
