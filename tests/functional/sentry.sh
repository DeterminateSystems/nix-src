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

# Sentry rejects crash dumps that are bigger than 200 MiB. When the
# stack pointer of the crashed thread is unusable (e.g. after a stack
# overflow), crashpad captures the signal stack instead, along with the
# rest of its mapping and any adjacent readable mappings, so a signal
# stack on the heap gives a crash dump of the size of the heap.
maxCrashDumpSize=$((200 * 1024 * 1024))

checkCrashDumpSize() {
    local size
    size=$(wc -c < "${envelopes[0]}")
    if ((size > maxCrashDumpSize)); then
        fail "Crash dump ${envelopes[0]} is $size bytes, which is more than Sentry accepts ($maxCrashDumpSize bytes)"
    fi
}

for type in segfault assert logic-error panic terminate abort stack-overflow; do
    if [[ $type = logic-error && $(uname) = Darwin ]]; then continue; fi
    # On macOS, crashpad catches crashes out of process rather than with
    # a signal handler, so the signal stack doesn't matter there.
    if [[ $type = stack-overflow && $(uname) != Linux ]]; then continue; fi

    rm -rf "$sentryDir"

    (! nix __crash "$type")

    if ! waitForCrashDump; then
        fail "No crash dump found in $sentryDir after crash"
    fi
    checkCrashDumpSize
done

# A stack overflow on a fiber of an evaluator worker thread must also
# produce a crash dump, which requires the worker thread to have an
# alternate signal stack. The crash dump includes the entire 60 MiB
# stack of the fiber.
if [[ $(uname) = Linux ]]; then
    rm -rf "$sentryDir"

    (! nix eval --eval-cores 4 --max-call-depth 10000000 --json --file ./fiber-stack-overflow.nix)

    if ! waitForCrashDump; then
        fail "No crash dump found in $sentryDir after a stack overflow on a fiber"
    fi
    checkCrashDumpSize
fi

# Exceeding the file size limit is an I/O error, not a crash, even
# though crashpad installs a crash handler for SIGXFSZ. The crashpad
# handler process inherits the limit, so leave room for a crash dump.
rm -rf "$sentryDir"

head -c 20000000 /dev/zero > "$TEST_ROOT/big"
# shellcheck disable=SC2016
expectStderr 1 bash -c 'ulimit -f 10000; nix nar pack "$1" > "$2"' _ "$TEST_ROOT/big" "$TEST_ROOT/big.nar" \
    | grepQuiet "File too large"

if waitForCrashDump; then
    fail "Unexpected crash dump after exceeding the file size limit"
fi

rm -rf "$sentryDir"

if nix shell --file ./simple.nix --command bash -c 'kill -SEGV $$'; then
    fail "Command did not segfault"
fi

if waitForCrashDump; then
    fail "Unexpected crash dump"
fi
