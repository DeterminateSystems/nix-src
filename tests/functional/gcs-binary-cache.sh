#!/usr/bin/env bash

source common.sh
TODO_NixOS # Clears the test store and starts an isolated daemon.

[[ $(type -p python3) ]] || skipTest "Python is not installed"
serverDir=$TEST_ROOT/gcs
mkdir -p "$serverDir"
python3 ./gcs-binary-cache-server.py "$serverDir" >"$serverDir/log" 2>&1 &
serverPid=$!
trap 'killDaemon; kill "$serverPid"' EXIT
for ((i = 0; i < 100; i++)); do
    [[ -e $serverDir/port ]] && break
    kill -0 "$serverPid" || fail "GCS test server exited"
    sleep 0.1
done
[[ -e $serverDir/port ]] || fail "GCS test server did not start"
endpoint="http://127.0.0.1:$(cat "$serverDir/port")"
export CLOUD_STORAGE_EMULATOR_ENDPOINT=$endpoint
GCE_METADATA_ROOT="127.0.0.1:$(cat "$serverDir/port")"
export GCE_METADATA_ROOT
unset GOOGLE_APPLICATION_CREDENTIALS

if ! nix store info --store gs://test/cache >"$serverDir/probe" 2>&1; then
    grepQuiet 'built without GCS support' "$serverDir/probe" && skipTest "GCS support is disabled"
    cat "$serverDir/probe" "$serverDir/log"
    fail "could not open GCS cache with metadata credentials"
fi

# A multi-chunk NAR exercises resumable upload retries and bounded buffering.
python3 -c 'import sys; open(sys.argv[1], "wb").write(b"0123456789abcdef" * (640 * 1024))' "$TEST_ROOT/payload"
outPath=$(nix-store --add "$TEST_ROOT/payload")
originalHash=$(nix hash path "$outPath")
nix copy --to 'gs://test/cache?compression=none' "$outPath"
clearStore
nix copy --from gcs://test/cache "$outPath" --no-check-sigs
[[ $(nix hash path "$outPath") == "$originalHash" ]]

# WIF with impersonation uses real ADC discovery, STS exchange and refresh.
printf %s test-subject-token > "$serverDir/subject-token"
jq -n --arg endpoint "$endpoint" --arg tokenFile "$serverDir/subject-token" '{
  type: "external_account",
  audience: "//iam.googleapis.com/projects/123/locations/global/workloadIdentityPools/test/providers/test",
  subject_token_type: "urn:ietf:params:oauth:token-type:jwt",
  token_url: ($endpoint + "/token"),
  service_account_impersonation_url: ($endpoint + "/impersonate"),
  credential_source: {file: $tokenFile}
}' > "$serverDir/adc.json"
export GOOGLE_APPLICATION_CREDENTIALS=$serverDir/adc.json
nix copy --to 'gs://test/wif%20cache?compression=none' "$outPath"
[[ $(jq -s '[.[] | select(.path == "/token")] | length' "$serverDir/requests") -gt 1 ]]
[[ $(jq -s '[.[] | select(.path == "/impersonate")] | length' "$serverDir/requests") -gt 1 ]]

# A 403 must not be treated as a cache miss and trigger cache creation.
expect 1 nix store info --store gs://denied/cache 2>"$serverDir/denied"
grepQuiet 'permission denied by test' "$serverDir/denied"
[[ $(jq -s '[.[] | select(.method != "GET" and (.path | contains("/b/denied/")))] | length' "$serverDir/requests") == 0 ]]

# Checksum failures must propagate to the caller.
clearStore
touch "$serverDir/corrupt"
expect 1 nix copy --from 'gs://test/wif%20cache' "$outPath" --no-check-sigs
rm "$serverDir/corrupt"
clearStore

# The daemon inherits ADC. A client with unusable ADC can still substitute.
cat >> "$test_nix_conf" <<EOF
substituters = gs://test/wif%20cache
require-sigs = false
EOF
startDaemon
trap 'killDaemon; kill "$serverPid"' EXIT
GOOGLE_APPLICATION_CREDENTIALS="$serverDir/does-not-exist" nix-store --realise "$outPath"
[[ $(nix hash path "$outPath") == "$originalHash" ]]
killDaemon

# ADC without impersonation uses the federated access token directly.
jq 'del(.service_account_impersonation_url)' "$serverDir/adc.json" > "$serverDir/direct.json"
GOOGLE_APPLICATION_CREDENTIALS="$serverDir/direct.json" nix store info --store gs://test/direct
jq -s -e 'any(.[]; .auth == "Bearer federated-token" and (.path | contains("/b/test/")))' "$serverDir/requests"

# An explicit broken ADC file must not silently fall back to the VM identity.
expect 1 env GOOGLE_APPLICATION_CREDENTIALS="$serverDir/does-not-exist" nix store info --store gs://test/bad-adc
