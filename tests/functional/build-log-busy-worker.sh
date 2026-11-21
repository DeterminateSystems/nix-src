#!/usr/bin/env bash

source common.sh

# Test that the output of builders ends up in the build log when
# they exit while the worker is busy with something else. On macOS,
# the output of a builder that has exited is discarded if it isn't
# read from the pseudoterminal within 0.6 s.

n=8

# On macOS, a thread reads the output of a builder into a pipe that
# holds 64 KiB. Also test builders that fill that pipe before they
# exit.
sizes=(64000 65000 66000 67000 68000 69000 70000)

# Running the pre-build hook blocks the worker.
hook=$TEST_ROOT/pre-build-hook.sh
cat > "$hook" <<EOF
#!/bin/sh
case "\$1" in
    *-build-log-busy-worker-slow.drv) sleep 4 ;;
esac
EOF
chmod +x "$hook"

args=(build-log-busy-worker.nix --arg n "$n" --arg sizes "[ ${sizes[*]} ]")

mapfile -t drvs < <(nix-instantiate "${args[@]}")
(( ${#drvs[@]} == 1 + n + ${#sizes[@]} ))

nix-build "${args[@]}" --no-out-link --max-jobs "${#drvs[@]}" --pre-build-hook "$hook"

missing=0
for drv in "${drvs[@]}"; do
    i=${drv%.drv}
    i=${i##*-}
    case "$drv" in
        *-slow.drv)
            continue
            ;;
        *-big-*)
            # $i bytes, a newline and the last line.
            if [[ "$(nix log "$drv" | wc -c)" -ne $((i + 15)) || "$(nix log "$drv" | tail -n1)" != "end of output" ]]; then
                echo "the log of '$drv' is incomplete" >&2
                missing=$((missing + 1))
            fi
            ;;
        *)
            if [[ "$(nix log "$drv")" != "output of build $i" ]]; then
                echo "the log of '$drv' is incomplete" >&2
                missing=$((missing + 1))
            fi
            ;;
    esac
done

if (( missing > 0 )); then
    fail "$missing of $((n + ${#sizes[@]})) build logs are incomplete"
fi
