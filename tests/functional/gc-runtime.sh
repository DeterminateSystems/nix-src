#!/usr/bin/env bash

source common.sh

case $system in
    *linux*)
        ;;
    *)
        skipTest "Not running Linux";
esac

TODO_NixOS

set -m # enable job control, needed for kill

programPath=$(nix-build --no-link ./gc-runtime.nix -A program)
environPath=$(nix-build --no-link ./gc-runtime.nix -A environ)
openPath=$(nix-build --no-link ./gc-runtime.nix -A open)

fifo="$TEST_ROOT/fifo"
mkfifo "$fifo"

echo "backgrounding program..."
export environPath
"$programPath"/program "$openPath"/open "$fifo" &
child=$!
echo PID=$child
cat "$fifo"

expectStderr 1 nix-store --delete "$openPath" | grepQuiet "Cannot delete path.*because it's referenced by the GC root '/proc/"

nix-store --gc

kill -- -$child

if ! test -e "$programPath"; then
    echo "running program was garbage collected!"
    exit 1
fi

if ! test -e "$environPath"; then
    echo "file in environment variable was garbage collected!"
    exit 1
fi

if ! test -e "$openPath"; then
    echo "opened file was garbage collected!"
    exit 1
fi

exit 0
