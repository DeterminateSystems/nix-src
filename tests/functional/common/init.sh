# shellcheck shell=bash

# for shellcheck
: "${test_nix_conf_dir?}" "${test_nix_conf?}"

if isTestOnNixOS; then

  mkdir -p "$test_nix_conf_dir" "$TEST_HOME"

  export NIX_USER_CONF_FILES="$test_nix_conf"
  mkdir -p "$test_nix_conf_dir" "$TEST_HOME"
  ! test -e "$test_nix_conf"
  cat > "$test_nix_conf" <<EOF
# TODO: this is not needed for all tests and prevents stable commands from be tested in isolation
experimental-features =
flake-registry = $TEST_ROOT/registry.json
show-trace = true
EOF

  # When we're doing everything in the same store, we need to bring
  # dependencies into context.
  sed -i "${_NIX_TEST_BUILD_DIR}/config.nix" \
    -e 's^\(shell\) = "/nix/store/\([^/]*\)/\(.*\)";^\1 = builtins.appendContext "/nix/store/\2" { "/nix/store/\2".path = true; } + "/\3";^' \
    -e 's^\(path\) = "/nix/store/\([^/]*\)/\(.*\)";^\1 = builtins.appendContext "/nix/store/\2" { "/nix/store/\2".path = true; } + "/\3";^' \
    ;

else

test -n "$TEST_ROOT"
# We would delete any daemon socket, so let's stop the daemon first.
killDaemon
# Destroy the test directory that may have persisted from previous runs
if [[ -e "$TEST_ROOT" ]]; then
    chmod -R u+w "$TEST_ROOT"
    rm -rf "$TEST_ROOT"
fi
mkdir -p "$TEST_ROOT"
mkdir "$TEST_HOME"

mkdir "$NIX_STORE_DIR"
mkdir "$NIX_LOCALSTATE_DIR"
mkdir -p "$NIX_LOG_DIR/drvs"
mkdir "$NIX_STATE_DIR"
mkdir "$NIX_CONF_DIR"

cat > "$NIX_CONF_DIR"/nix.conf <<EOF
build-users-group =
keep-derivations = false
sandbox = false
experimental-features =
gc-reserved-space = 0
substituters =
flake-registry = $TEST_ROOT/registry.json
show-trace = true
include nix.conf.extra
trusted-users = $(whoami)
${_NIX_TEST_EXTRA_CONFIG:-}
EOF

cat > "$NIX_CONF_DIR"/nix.conf.extra <<EOF
fsync-metadata = false
!include nix.conf.extra.not-there
EOF

# Initialise the database.
# The flag itself does nothing, but running the command touches the store
nix-store --init
# Sanity check
test -e "$NIX_STATE_DIR"/db/db.sqlite

fi # !isTestOnNixOS
