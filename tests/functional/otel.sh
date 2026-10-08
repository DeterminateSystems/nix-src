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

# The helpers below look at all the uploads received since the last
# `clearUploads`. Note that a process may spread its spans over
# several uploads (the exporter uploads periodically as well as on
# exit), so spans are selected by name rather than by upload.

clearUploads() {
    rm -f "$sinkDir"/*.body "$sinkDir"/*.headers
}

# Return the number of uploads.
uploads() {
    find "$sinkDir" -name '*.body' | wc -l
}

# Apply a jq filter to every span of every upload.
spans() {
    jq -r ".resourceSpans[0].scopeSpans[0].spans[] | $1" "$sinkDir"/*.body
}

# Return a field of the span with the given name, e.g. `.kind`.
span() {
    spans "select(.name == \"$1\") | $2"
}

# Return the value of a string attribute of the span with the given
# name.
attr() {
    span "$1" ".attributes[] | select(.key == \"$2\") | .value.stringValue"
}

# Return the service names of the uploads, one per line.
services() {
    jq -r '.resourceSpans[0].resource.attributes[] | select(.key == "service.name") | .value.stringValue' "$sinkDir"/*.body | sort
}

# A successful command produces a root span named after it, with no
# status.
clearUploads
[[ $(nix eval --expr '1 + 2') = 3 ]]
[[ $(services) = nix ]]
[[ $(span "nix eval" .status) = null ]]
[[ $(span "nix eval" .kind) = 1 ]]

# The headers from the configuration should arrive percent-decoded,
# and the upload of the trace itself must not be part of the trace.
[[ $(jq -r .authorization "$sinkDir"/*.headers) = "Bearer secret" ]]
[[ $(jq -r .traceparent "$sinkDir"/*.headers) = null ]]

# A failing command marks the root span as failed, with the error
# message.
clearUploads
expect 1 nix eval --expr '1 + "x"'
[[ $(span "nix eval" .status.code) = 2 ]]
span "nix eval" .status.message | grepQuiet "cannot add a string to an integer"

# Legacy commands are traced as well, under their own name.
clearUploads
[[ $(nix-instantiate --eval --expr '1 + 2') = 3 ]]
[[ $(span nix-instantiate .kind) = 1 ]]

# Setting an endpoint in the environment enables tracing even if
# `otlp` is disabled, and takes precedence over the configuration.
clearUploads
[[ $(NIX_CONFIG="otlp = false" nix eval --expr '1 + 2') = 3 ]]
[[ $(uploads) = 0 ]]
[[ $(OTEL_EXPORTER_OTLP_ENDPOINT="http://127.0.0.1:$(cat "$sinkDir/port")" NIX_CONFIG="otlp = false" nix eval --expr '1 + 2') = 3 ]]
[[ $(span "nix eval" .kind) = 1 ]]

# A build produces a `Build` span carrying the derivation's path, name
# and version as separate attributes.
# shellcheck disable=SC2016 # `$out` is for the Nix builder, not the shell.
drvPath=$(nix-instantiate --expr 'with import ./config.nix; mkDerivation { name = "foo-1.2"; buildCommand = "echo > $out"; }')
clearUploads
# Note: without a network (e.g. in the Nix sandbox), `nix` turns off
# substitution unless it's requested explicitly, and then there is no
# substitution span.
nix build --no-link --substitute "$drvPath^*"
[[ $(attr Build nix.drv.path) = "$drvPath" ]]
[[ $(attr Build nix.drv.name) = foo ]]
[[ $(attr Build nix.drv.version) = 1.2 ]]
[[ $(attr Build nix.build.status) = Built ]]
[[ $(span Build .status.code) = null ]]
# Not being able to substitute the path is the normal prelude to
# building it, not an error.
[[ $(attr SubstitutionGoal nix.build.status) = NoSubstituters ]]
[[ $(span SubstitutionGoal .status.code) = null ]]

# A failed build marks its `Build` span as failed, with the build's
# status and error message. Note that the failure doesn't throw, so
# this relies on the build result rather than on stack unwinding.
# shellcheck disable=SC2016 # `$out` is for the Nix builder, not the shell.
drvPath=$(nix-instantiate --expr 'with import ./config.nix; mkDerivation { name = "bar-1.2"; buildCommand = "echo something went wrong >&2; exit 1"; }')
clearUploads
expect 1 nix build --no-link "$drvPath^*"
[[ $(attr Build nix.build.status) = PermanentFailure ]]
[[ $(span Build .status.code) = 2 ]]
span Build .status.message | grepQuiet "builder failed with exit code 1"

# A daemon reached via `ssh-ng://` (here without real SSH, since the
# host is `localhost`) exports its own spans, in the client's trace.
# The client and the daemon upload independently, so wait for both.
clearUploads
nix store info --store "ssh-ng://localhost?remote-store=$TEST_ROOT/other-store" > /dev/null
for ((i = 0; i < 100; i++)); do
    [[ $(uploads) -ge 2 ]] && break
    sleep 0.1
done
[[ $(services) = $'nix\nnix-daemon' ]]
[[ $(span "nix store info" .kind) = 1 ]]
[[ $(span "daemon connection" .kind) = 2 ]] # SERVER
[[ $(span "daemon connection" .traceId) = $(span "nix store info" .traceId) ]]
[[ $(span "daemon connection" .parentSpanId) = $(span "nix store info" .spanId) ]]

# The root span is parented to the trace context in `TRACEPARENT`,
# which is how remote builds via `build-remote` are linked to the
# trace of the `nix` process that runs it.
clearUploads
[[ $(TRACEPARENT=00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01 nix eval --expr '1 + 2') = 3 ]]
[[ $(span "nix eval" .traceId) = 0af7651916cd43dd8448eb211c80319c ]]
[[ $(span "nix eval" .parentSpanId) = b7ad6b7169203331 ]]
[[ $(span "nix eval" .kind) = 1 ]] # INTERNAL, not SERVER
