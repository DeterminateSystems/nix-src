#!/usr/bin/env bash

source common.sh

# Enable sentry with a fake endpoint.
unset NIX_SENTRY_ENDPOINT
echo -n "file://$TEST_ROOT/sentry-endpoint" > "$test_nix_conf_dir/sentry-endpoint"

ulimit -c 0

sentryDir="$TEST_HOME/.cache/nix/sentry"

nix --version
if ! [[ -d $sentryDir ]]; then
    skipTest "not built with sentry support"
fi

# The crashpad handler captures a crash by ptrace-attaching to the crashing
# process. Yama ptrace_scope >= 2 restricts ptrace to CAP_SYS_PTRACE, which an
# unprivileged test run does not have (and PR_SET_PTRACER only helps at scope
# 1), so no minidump can ever be written on such kernels — e.g. hardened hosts
# or CI sandboxes.
if [[ -r /proc/sys/kernel/yama/ptrace_scope && $(< /proc/sys/kernel/yama/ptrace_scope) -ge 2 ]]; then
    skipTest "ptrace is restricted by Yama (kernel.yama.ptrace_scope >= 2)"
fi

waitForCrashDump() {
    local i
    for ((i = 0; i < 10; i++)); do
        envelopes=("$sentryDir"/pending/*.dmp)
        if [[ -e "${envelopes[0]}" ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

for type in segfault assert logic-error panic terminate abort; do
    if [[ $type = logic-error && $(uname) = Darwin ]]; then continue; fi

    rm -rf "$sentryDir"

    (! nix __crash "$type")

    if ! waitForCrashDump; then
        fail "No crash dump found in $sentryDir after crash"
    fi
done

rm -rf "$sentryDir"

if nix shell --file ./simple.nix --command bash -c 'kill -SEGV $$'; then
    fail "Command did not segfault"
fi

if waitForCrashDump; then
    fail "Unexpected crash dump"
fi
