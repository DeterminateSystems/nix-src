#!/usr/bin/env bash

# Test that the SSH master process (`ssh -M -N`) of an `ssh-ng://`
# store and its temporary directory don't outlive Nix, even if the
# store is never destroyed (e.g. because a reference to it is leaked).

source common.sh

# A fake `ssh` that runs the remote command locally. A master records
# its PID and the directory containing its control socket, and keeps
# running until it's killed.
export SSH_MASTERS=$TEST_ROOT/ssh-masters
mkdir -p "$TEST_ROOT/fake-ssh"
{
    echo "#!$(type -P bash)"
    cat <<'EOF'
master=
while [[ $# -gt 0 ]]; do
    case "$1" in
        -O) exit 1 ;; # `ssh -O check`: no master is running
        -M) master=1 ;;
        -S) shift; socket=$1 ;;
        --) shift; break ;;
    esac
    shift
done
if [[ -n $master ]]; then
    echo "$$ $(dirname "$socket")" >> "$SSH_MASTERS"
    echo started
    exec sleep 1000 < /dev/null > /dev/null 2>&1
fi
exec "$@"
EOF
} > "$TEST_ROOT/fake-ssh/ssh"
chmod +x "$TEST_ROOT/fake-ssh/ssh"
export PATH=$TEST_ROOT/fake-ssh:$PATH

# Use more than one connection, since otherwise there is no master.
store="ssh-ng://fakehost?remote-store=$TEST_ROOT/other-store&max-connections=2"

# Check that the masters that were started have been killed, and their
# temporary directories deleted.
checkMasters() {
    [[ -s $SSH_MASTERS ]] || fail "no SSH master was started"
    local pid dir i
    while read -r pid dir; do
        # Give the killed process some time to be reaped.
        for ((i = 0; i < 50; i++)); do
            kill -0 "$pid" 2> /dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2> /dev/null; then
            kill "$pid"
            fail "SSH master $pid is still running"
        fi
        [[ ! -e $dir ]] || fail "temporary directory $dir still exists"
    done < "$SSH_MASTERS"
    rm "$SSH_MASTERS"
}

# A successful evaluation that copies a path to the store.
echo foo > "$TEST_ROOT/foo"
nix eval --store "$store" --impure --raw --expr "\"\${$TEST_ROOT/foo}\"" | grepQuiet foo
checkMasters

# An evaluation error. The failed value keeps the error, and thus the
# source accessor and the store, alive.
cat > "$TEST_ROOT/throw.nix" <<EOF
builtins.seq (builtins.toFile "foo" "bar") (throw "this is an error")
EOF
expectStderr 1 nix eval --store "$store" --impure -f "$TEST_ROOT/throw.nix" | grepQuiet "this is an error"
checkMasters
