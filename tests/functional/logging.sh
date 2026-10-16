#!/usr/bin/env bash

source common.sh

TODO_NixOS

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
builder=$TEST_ROOT/builder
echo -e "#!/bin/sh\nmkdir \$out" > "$builder"
outp="$(nix-build -E \
    'with import '"${config_nix}"'; mkDerivation { name = "fnord"; builder = '"$builder"'; }' \
    --out-link "$TEST_ROOT/result")"

test -d "$outp"

nix log "$outp"

if isDaemonNewer "2.26"; then
    # Build works despite ill-formed structured build log entries.
    expectStderr 0 nix build -f ./logging/unusual-logging.nix --no-link | grepQuiet 'warning: Unable to handle a JSON message from the derivation builder:'
fi

# Test json-log-path.
clearStore
nix build -vv --file dependencies.nix --no-link --json-log-path "$TEST_ROOT/log.json" 2>&1 | grepQuiet 'building.*dependencies-top.drv'
grep '{"action":"start","fields":\[".*-dependencies-top.drv","",1,1\],"id":.*,"level":3,"parent":[1-9]' "$TEST_ROOT/log.json" >&2
grep -E '{"action":"result","id":[^,]+,"payload":{"builtOutputs":{"out":{"outPath":"[^-]+-dependencies-top".*"status":"Built".*"success":true' "$TEST_ROOT/log.json" >&2
(( $(grep -c '{"action":"msg","level":5,"msg":"executing builder .*"}' "$TEST_ROOT/log.json" ) == 5 ))

rm "$TEST_ROOT/log.json"
expect 1 nix build -vv --file timeout.nix silent --timeout 1 --no-link --json-log-path "$TEST_ROOT/log.json"
grep -E '{"action":"result","id":[^,]+,"payload":{"errorMsg":"timed out.*",.*"startTime":[1-9][0-9]*,"status":"TimedOut","stopTime":0,"success":false,' "$TEST_ROOT/log.json" >&2

# Check that all log entries have the same session ID.
sid=$(head -n1 < "$TEST_ROOT/log.json" | jq -r '.sid')
[[ -n "$sid" && "$sid" != "null" ]]
(( $(jq -s --arg sid "$sid" '[.[] | select(.sid != $sid)] | length' < "$TEST_ROOT/log.json") == 0 ))

# Test whether setting an explicit session ID works.
nix store info --json-log-path "$TEST_ROOT/log2.json" --session-id "foo"
(( $(jq -s 'length' < "$TEST_ROOT/log2.json") > 0 ))
(( $(jq -s --arg sid foo '[.[] | select(.sid != $sid)] | length' < "$TEST_ROOT/log2.json") == 0 ))

# Test that string-named activities (including those forwarded by the
# daemon) appear in the JSON log with their name and metadata.
if isDaemonNewer "2.36pre"; then
    clearStore
    clearBinaryCache
    path=$(nix-build dependencies.nix --no-out-link)
    nix key generate-secret --key-name test-1 > "$TEST_ROOT/sk"
    nix copy --to "file://$cacheDir?secret-key=$TEST_ROOT/sk" "$path"
    clearStore
    clearCacheCache
    # Use the daemon so that we test forwarding of string-named activities.
    startDaemon
    nix build --no-link --substitute --substituters "file://$cacheDir" --trusted-public-keys "$(nix key convert-secret-to-public < "$TEST_ROOT/sk")" \
        --json-log-path "$TEST_ROOT/log3.json" "$path"
    (( $(jq -s --arg path "$path" '[.[] | select(.action == "start" and .type == 10113 and .name == "SubstitutionGoal" and .payload."nix.store.path" == $path)] | length' < "$TEST_ROOT/log3.json") == 1 ))
fi

# Test the `tui` log format. Since stderr is not a terminal, it prints
# plain log messages, like the `bar` log format.
clearStore
nix build --log-format tui -L --file dependencies.nix --no-link 2>&1 | tee "$TEST_ROOT/tui.log"
grepQuiet 'building.*dependencies-top.drv' "$TEST_ROOT/tui.log"
grepQuiet 'dependencies-top> FOO' "$TEST_ROOT/tui.log"
(! grepQuiet $'\e' "$TEST_ROOT/tui.log")
