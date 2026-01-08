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

# Start the HTTP server on a random port using our test server script
TEST_SERVER="$_NIX_TEST_SOURCE_DIR/binary-cache-auth-errors_test-http-server.py"
CACHE_DIR="$cacheDir" RETURN_CODE=200 python3 "$TEST_SERVER" > "$TEST_ROOT/port" 2>&1 &
SERVER_PID=$!
sleep 1
HTTP_PORT=$(cat "$TEST_ROOT/port")

cleanup() {
    kill $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

# Test 1: Normal operation (200 OK) should work
clearStore
nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-200"
[ -x "$outPath/program" ]

# Stop the server and restart with 404 errors
kill $SERVER_PID
sleep 1

# Test 2: 404 errors should say "does not exist in binary cache"
clearStore
CACHE_DIR="$cacheDir" RETURN_CODE=404 python3 "$TEST_SERVER" > "$TEST_ROOT/port" 2>&1 &
SERVER_PID=$!
sleep 1
HTTP_PORT=$(cat "$TEST_ROOT/port")

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
CACHE_DIR="$cacheDir" RETURN_CODE=401 python3 "$TEST_SERVER" > "$TEST_ROOT/port" 2>&1 &
SERVER_PID=$!
sleep 1
HTTP_PORT=$(cat "$TEST_ROOT/port")

if nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-401"; then
    echo "Expected substitution to fail with 401"
    exit 1
fi

grepQuiet "HTTP error 401" "$TEST_ROOT/log-401"
grepQuiet "access token" "$TEST_ROOT/log-401"

# Verify it does NOT say "does not exist" for 401 errors
if grep -q "does not exist in binary cache" "$TEST_ROOT/log-401"; then
    echo "ERROR: 401 error incorrectly says 'does not exist in binary cache'"
    echo "Log contents:"
    cat "$TEST_ROOT/log-401"
    exit 1
fi

# Stop the server and restart with 403 errors
kill $SERVER_PID
sleep 1

# Test 4: 403 errors should also mention authentication and NOT say "does not exist"
clearStore
CACHE_DIR="$cacheDir" RETURN_CODE=403 python3 "$TEST_SERVER" > "$TEST_ROOT/port" 2>&1 &
SERVER_PID=$!
sleep 1
HTTP_PORT=$(cat "$TEST_ROOT/port")

if nix-store --substituters "http://127.0.0.1:$HTTP_PORT" --no-require-sigs -r "$outPath" 2>&1 | tee "$TEST_ROOT/log-403"; then
    echo "Expected substitution to fail with 403"
    exit 1
fi

grepQuiet "HTTP error 403" "$TEST_ROOT/log-403"
grepQuiet "access token" "$TEST_ROOT/log-403"

# Verify it does NOT say "does not exist" for 403 errors
if grep -q "does not exist in binary cache" "$TEST_ROOT/log-403"; then
    echo "ERROR: 403 error incorrectly says 'does not exist in binary cache'"
    echo "Log contents:"
    cat "$TEST_ROOT/log-403"
    exit 1
fi

echo "All HTTP error tests passed!"
