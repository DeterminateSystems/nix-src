#!/usr/bin/env bash

source common.sh

touch "$TEST_ROOT/foo" -t 202211111111

# The path fetcher does not return lastModified.
[[ "$(nix eval --impure --expr "(builtins.fetchTree \"path://$TEST_ROOT/foo\") ? lastModifiedDate")" = false ]]

# Check that we can override lastModified for "path:" inputs.
[[ "$(nix eval --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$TEST_ROOT/foo\"; lastModified = 123; }).lastModified")" = 123 ]]
