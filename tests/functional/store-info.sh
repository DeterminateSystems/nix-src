#!/usr/bin/env bash

source common.sh

STORE_INFO=$(nix store info 2>&1)
LEGACY_STORE_INFO=$(nix store ping 2>&1) # alias to nix store info
STORE_INFO_JSON=$(nix store info --json)

echo "$STORE_INFO" | grep "Store URL: ${NIX_REMOTE}"
echo "$LEGACY_STORE_INFO" | grep "Store URL: ${NIX_REMOTE}"

if [[ -v NIX_DAEMON_PACKAGE ]] && isDaemonNewer "2.7.0pre20220126"; then
    DAEMON_VERSION=$("$NIX_DAEMON_PACKAGE"/bin/nix daemon --version | sed 's/.*) //')
    echo "$STORE_INFO" | grep "Version: $DAEMON_VERSION"
    [[ "$(echo "$STORE_INFO_JSON" | jq -r ".version")" == "$DAEMON_VERSION" ]]
fi


expect 127 NIX_REMOTE=unix:"$PWD"/store nix store info || \
    fail "nix store info on a non-existent store should fail"

TODO_NixOS

[[ "$(echo "$STORE_INFO_JSON" | jq -r ".url")" == "${NIX_REMOTE:-local}" ]]
