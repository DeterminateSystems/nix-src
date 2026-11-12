#!/usr/bin/env bash

# Test that a build started through `BuildDerivation` protects its valid
# input sources from a concurrent garbage collector.
#
# `BuildDerivation` receives a `BasicDerivation` whose `.drv` file is not
# in the building store, so nothing roots its inputs unless the build
# itself does. We use a remote build to `ssh://localhost` (which runs
# `nix-store --serve` directly, without ssh) to send `BuildDerivation` to
# the test store: the client holds the derivation in a separate store and
# doesn't root anything on the builder's side. The builder blocks on a
# FIFO while we run a complete GC on the test store.
#
# The input is a file that refers to another store path, so the test
# also checks that the input's closure survives. Its store path is passed
# through `passAsFile`, so it doesn't appear in the builder's environment,
# where the GC's runtime root scan would find it.

source common.sh

TODO_NixOS

clientStore=$TEST_ROOT/client

rm -rf "$TEST_ROOT/dep"
echo hello > "$TEST_ROOT/dep"

# Build a derivation with `nix-store --serve` on `remoteStore`, run a GC
# on the test store while the builder is blocked, and check that the
# input closure survived the GC and that the build succeeded.
checkBuildInput() {
    local remoteStore=$1

    # The input and its dependency are valid in both stores, but nothing
    # roots them.
    local dep input inputExpr
    dep=$(nix-store --add "$TEST_ROOT/dep")
    [[ $(nix-store --store "$clientStore" --add "$TEST_ROOT/dep") == "$dep" ]]
    inputExpr="builtins.toFile \"input\" (builtins.storePath \"$dep\")"
    input=$(nix-instantiate --eval --read-write-mode --json --expr "$inputExpr" | tr -d '"')
    [[ $(nix-instantiate --store "$clientStore" --eval --read-write-mode --json --expr "$inputExpr" | tr -d '"') == "$input" ]]
    [[ $(nix-store --query --references "$input") == "$dep" ]]

    local drv
    drv=$(nix-instantiate --store "$clientStore" --expr "
      derivation {
        name = \"gc-build-input\";
        system = builtins.currentSystem;
        builder = \"$BASH\";
        args = [ \"-c\" ''
          echo ready > \"\$ready\"
          read -r _ < \"\$resume\"
          IFS= read -r input < \"\$inputPath\" || [[ -n \$input ]]
          IFS= read -r dep < \"\$input\" || [[ -n \$dep ]]
          IFS= read -r content < \"\$dep\" || exit 1
          echo \"\$content\" > \"\$out\"
        '' ];
        input = $inputExpr;
        passAsFile = [ \"input\" ];
        ready = \"$TEST_ROOT/ready\";
        resume = \"$TEST_ROOT/resume\";
        remoteStore = \"$remoteStore\";
      }
    ")

    rm -f "$TEST_ROOT/ready" "$TEST_ROOT/resume"
    mkfifo "$TEST_ROOT/ready" "$TEST_ROOT/resume"

    nix-store --store "$clientStore" --realise "$drv" --max-jobs 0 \
        --builders "ssh://localhost?remote-store=$remoteStore - - 1 1" \
        > "$TEST_ROOT/build.out" &
    local pid=$!

    # Wait for the builder, but don't hang if the build fails before it starts.
    local readyFd
    exec {readyFd}<>"$TEST_ROOT/ready"
    while ! read -r -t 1 -u "$readyFd" _; do
        kill -0 "$pid" 2>/dev/null || fail "$remoteStore: build exited before the builder started"
    done
    exec {readyFd}<&-
    nix-store --gc
    local deleted=()
    for p in "$input" "$dep"; do
        [[ -e $p ]] || deleted+=("$p")
    done
    echo continue > "$TEST_ROOT/resume"
    local status=0
    wait "$pid" || status=$?
    if (( ${#deleted[@]} )); then
        fail "$remoteStore: GC deleted ${deleted[*]} during the build"
    fi
    [[ $status == 0 ]] || fail "$remoteStore: build failed"
    rm -f "$TEST_ROOT/result"
    nix-store --store "$clientStore" --dump "$(cat "$TEST_ROOT/build.out")" | nix-store --restore "$TEST_ROOT/result"
    [[ $(cat "$TEST_ROOT/result") == hello ]] || fail "$remoteStore: wrong build output"
}

# `nix-store --serve` builds in its own process.
if [[ ${NIX_REMOTE:-} != daemon ]]; then
    checkBuildInput local
    startDaemon
fi

# `nix-store --serve` forwards `BuildDerivation` to the daemon.
checkBuildInput daemon

if [[ ${NIX_REMOTE:-} != daemon ]]; then
    killDaemon
fi
