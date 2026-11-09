#!/usr/bin/env bash

source common.sh

TODO_NixOS

# Computing a closure through the daemon must work with a single
# connection. This used to deadlock because RemoteStore::queryPathInfos()
# invoked its callback while holding the connection, and the callback in
# computeFSClosure() uses the store again.

# A closure that is three levels deep.
outPath=$(nix-build --no-out-link dependencies.nix)
expected=$(nix-store --query --requisites "$outPath" | sort)

# A derivation without references.
drvPath=$(nix-instantiate --expr 'derivation { name = "no-refs"; system = builtins.currentSystem; builder = "/bin/sh"; }')

startDaemon

store="unix://$NIX_DAEMON_SOCKET_PATH?max-connections=1"

# Fail rather than hang if this deadlocks. A deadlocked client may not
# respond to SIGTERM, so kill it after a grace period.
closure=$(timeout -k 10 60 nix path-info --store "$store" --recursive "$outPath" | sort)
[[ "$closure" = "$expected" ]]

# With `--include-outputs`, the callback also queries the outputs of
# the derivation, which needs a connection as well.
closure=$(timeout -k 10 60 nix-store --store "$store" --query --requisites --include-outputs "$drvPath")
[[ "$closure" = "$drvPath" ]]
