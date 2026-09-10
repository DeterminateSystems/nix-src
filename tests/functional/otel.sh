#!/usr/bin/env bash

# Test OpenTelemetry trace export: run a few commands against a local
# collector that just saves whatever it receives, then check the
# resulting OTLP/JSON.

source common.sh

# Only meaningful if Nix was built with OpenTelemetry support.
nix config show | grepQuiet '^otlp ' || skipTest "Nix was built without OpenTelemetry support"

[[ $(type -p python3) ]] || skipTest "Python is not installed"

sinkDir=$TEST_ROOT/otel
mkdir -p "$sinkDir"

python3 ./otel-sink.py "$sinkDir" &
sinkPid=$!
trap 'kill "$sinkPid"' EXIT

for ((i = 0; i < 100; i++)); do
    [[ -e $sinkDir/port ]] && break
    kill -0 "$sinkPid" || fail "the collector died unexpectedly"
    sleep 0.1
done
[[ -e $sinkDir/port ]] || fail "the collector didn't start"

cat >> "$test_nix_conf" <<EOF
otlp = true
otlp-endpoint = http://127.0.0.1:$(cat "$sinkDir/port")
otlp-compression = none
otlp-headers = authorization=Bearer%20secret
EOF

# Return the body of the n-th upload, as JSON.
body() {
    cat "$sinkDir/$1.body"
}

# Return the value of the n-th upload's `.[0]`-th span field.
span() {
    body "$1" | jq -r ".resourceSpans[0].scopeSpans[0].spans[0]$2"
}

# Return the value of a string attribute of the span with the given
# name in the n-th upload.
attr() {
    body "$1" | jq -r ".resourceSpans[0].scopeSpans[0].spans[] | select(.name == \"$2\") | .attributes[] | select(.key == \"$3\") | .value.stringValue"
}

# A successful command produces a root span named after it, with no
# status.
[[ $(nix eval --expr '1 + 2') = 3 ]]
[[ $(body 0 | jq -r '.resourceSpans[0].resource.attributes[] | select(.key == "service.name") | .value.stringValue') = nix ]]
[[ $(span 0 .name) = "nix eval" ]]
[[ $(span 0 .status) = null ]]
[[ $(span 0 .kind) = 1 ]]

# The headers from the configuration should arrive percent-decoded,
# and the upload of the trace itself must not be part of the trace.
[[ $(jq -r .authorization "$sinkDir/0.headers") = "Bearer secret" ]]
[[ $(jq -r .traceparent "$sinkDir/0.headers") = null ]]

# A failing command marks the root span as failed, with the error
# message.
expect 1 nix eval --expr '1 + "x"'
[[ $(span 1 .name) = "nix eval" ]]
[[ $(span 1 .status.code) = 2 ]]
span 1 .status.message | grepQuiet "cannot add a string to an integer"

# Legacy commands are traced as well, under their own name.
[[ $(nix-instantiate --eval --expr '1 + 2') = 3 ]]
[[ $(span 2 .name) = "nix-instantiate" ]]

# Setting an endpoint in the environment enables tracing even if
# `otlp` is disabled, and takes precedence over the configuration.
[[ $(NIX_CONFIG="otlp = false" nix eval --expr '1 + 2') = 3 ]]
[[ ! -e $sinkDir/3.body ]]
[[ $(OTEL_EXPORTER_OTLP_ENDPOINT="http://127.0.0.1:$(cat "$sinkDir/port")" NIX_CONFIG="otlp = false" nix eval --expr '1 + 2') = 3 ]]
[[ $(span 3 .name) = "nix eval" ]]

# A build produces a `Build` span carrying the derivation's path, name
# and version as separate attributes.
# shellcheck disable=SC2016 # `$out` is for the Nix builder, not the shell.
drvPath=$(nix-instantiate --expr 'with import ./config.nix; mkDerivation { name = "foo-1.2"; buildCommand = "echo > $out"; }')
nix build --no-link "$drvPath^*"
[[ $(attr 5 Build nix.drv.path) = "$drvPath" ]]
[[ $(attr 5 Build nix.drv.name) = foo ]]
[[ $(attr 5 Build nix.drv.version) = 1.2 ]]
