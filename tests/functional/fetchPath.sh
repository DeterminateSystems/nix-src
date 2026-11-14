#!/usr/bin/env bash

source common.sh

touch "$TEST_ROOT/foo" -t 202211111111

# The path fetcher does not return lastModified.
[[ "$(nix eval --impure --expr "(builtins.fetchTree \"path://$TEST_ROOT/foo\") ? lastModifiedDate")" = false ]]

# Check that we can override lastModified for "path:" inputs.
[[ "$(nix eval --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$TEST_ROOT/foo\"; lastModified = 123; }).lastModified")" = 123 ]]

# lastModifiedDate is lastModified in UTC.
[[ "$(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$TEST_ROOT/foo\"; lastModified = 689742900; }).lastModifiedDate")" = 19911110031500 ]]

# A lastModified that can't be converted to a date is an error, not a crash.
expectStderr 1 nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$TEST_ROOT/foo\"; lastModified = 9223372036854775807; }).lastModifiedDate" \
    | grepQuiet "cannot convert lastModified timestamp 9223372036854775807 to UTC"
