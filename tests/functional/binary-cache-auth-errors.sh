#!/usr/bin/env bash

# Test that HTTP binary cache errors are properly distinguished:
# - 404 errors should say "does not exist in binary cache"
# - 401/403 errors should mention authentication and token expiration

source common.sh

TODO_NixOS

needLocalStore "'--no-require-sigs' can't be used with the daemon"

# Create a binary cache with some content
clearStore
clearCache
outPath=$(nix-build dependencies.nix --no-out-link)
nix copy --to "file://$cacheDir" "$outPath"

# Function to start the test HTTP server with a specific return code
TEST_SERVER="$_NIX_TEST_SOURCE_DIR/binary-cache-auth-errors_test-http-server.py"
startHttpServer() {
    local return_code=$1
    CACHE_DIR="$cacheDir" RETURN_CODE="$return_code" python3 "$TEST_SERVER" > "$TEST_ROOT/port" 2>&1 &
    SERVER_PID=$!
    sleep 1
    HTTP_PORT=$(cat "$TEST_ROOT/port")
}

# Function to verify a pattern does NOT appear in a log file
expectNoMatch() {
    local pattern=$1
    local logfile=$2
    local error_code=$3

    if grep -q "$pattern" "$logfile"; then
        echo "ERROR: $error_code error incorrectly says '$pattern'"
        echo "Log contents:"
        cat "$logfile"
        exit 1
    fi
}

cleanup() {
    kill $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

# Start the HTTP server on a random port
startHttpServer 200

# Test 1: Normal operation (200 OK) should work
clearStore
nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-200"
[ -x "$outPath/program" ]
expectNoMatch "does not exist in binary cache" "$TEST_ROOT/log-200" "200"
expectNoMatch "HTTP error" "$TEST_ROOT/log-200" "200"

# Stop the server and restart with 404 errors
kill $SERVER_PID
sleep 1

# Test 2: 404 errors should say "does not exist in binary cache"
clearStore
startHttpServer 404

if nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-404"; then
    echo "Expected substitution to fail with 404"
    exit 1
fi

echo "=== 404 Log contents ==="
cat "$TEST_ROOT/log-404"
echo "=== End 404 Log ==="

grepQuiet "does not exist in binary cache" "$TEST_ROOT/log-404"

# Stop the server and restart with 401 errors
kill $SERVER_PID
sleep 1

# Test 3: 401 errors should mention authentication and NOT say "does not exist"
clearStore
startHttpServer 401

if nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-401"; then
    echo "Expected substitution to fail with 401"
    exit 1
fi

grepQuiet "HTTP error 401" "$TEST_ROOT/log-401"
grepQuiet "access token" "$TEST_ROOT/log-401"

# Verify it does NOT say "does not exist" for 401 errors
expectNoMatch "does not exist in binary cache" "$TEST_ROOT/log-401" "401"

# Stop the server and restart with 403 errors
kill $SERVER_PID
sleep 1

# Test 4: 403 errors should also mention authentication and NOT say "does not exist"
clearStore
startHttpServer 403

if nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-403"; then
    echo "Expected substitution to fail with 403"
    exit 1
fi

grepQuiet "HTTP error 403" "$TEST_ROOT/log-403"
grepQuiet "access token" "$TEST_ROOT/log-403"

# Verify it does NOT say "does not exist" for 403 errors
expectNoMatch "does not exist in binary cache" "$TEST_ROOT/log-403" "403"

echo "All HTTP error tests passed!"
