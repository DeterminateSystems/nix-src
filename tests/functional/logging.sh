#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStore

path=$(nix-build dependencies.nix --no-out-link)

# Test nix-store -l.
[ "$(nix-store -l "$path")" = FOO ]

# Test compressed logs.
clearStore
rm -rf "$NIX_LOG_DIR"
(! nix-store -l "$path")
nix-build dependencies.nix --no-out-link --compress-build-log
[ "$(nix-store -l "$path")" = FOO ]

# test whether empty logs work fine with `nix log`.
builder="$(realpath "$(mktemp)")"
echo -e "#!/bin/sh\nmkdir \$out" > "$builder"
outp="$(nix-build -E \
    'with import '"${config_nix}"'; mkDerivation { name = "fnord"; builder = '"$builder"'; }' \
    --out-link "$(mktemp -d)/result")"

test -d "$outp"

nix log "$outp"

if isDaemonNewer "2.26"; then
    # Build works despite ill-formed structured build log entries.
    expectStderr 0 nix build -f ./logging/unusual-logging.nix --no-link | grepQuiet 'warning: Unable to handle a JSON message from the derivation builder:'
fi

# Test json-log-path.
clearStore
nix build -vv --file dependencies.nix --no-link --json-log-path "$TEST_ROOT/log.json" 2>&1 | grepQuiet 'building.*dependencies-top.drv'
grep '{"action":"start","fields":\[".*-dependencies-top.drv","",1,1\],"id":.*,"level":3,"parent":0' "$TEST_ROOT/log.json" >&2
grep -E '{"action":"result","id":[^,]+,"payload":{"builtOutputs":{"out":{"dependentRealisations":\{\},"id":"[^"]+","outPath":"[^-]+-dependencies-top".*"status":"Built".*"success":true' "$TEST_ROOT/log.json" >&2
(( $(grep -c '{"action":"msg","level":5,"msg":"executing builder .*"}' "$TEST_ROOT/log.json" ) == 5 ))

# Check that all log entries have the same session ID.
sid=$(head -n1 < "$TEST_ROOT/log.json" | jq -r '.sid')
[[ -n "$sid" && "$sid" != "null" ]]
(( $(jq -s --arg sid "$sid" '[.[] | select(.sid != $sid)] | length' < "$TEST_ROOT/log.json") == 0 ))

# Test whether setting an explicit session ID works.
nix store info --json-log-path "$TEST_ROOT/log2.json" --session-id "foo"
(( $(jq -s 'length' < "$TEST_ROOT/log2.json") > 0 ))
(( $(jq -s --arg sid foo '[.[] | select(.sid != $sid)] | length' < "$TEST_ROOT/log2.json") == 0 ))
