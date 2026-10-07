# The builder for running a test program (see `runTest` in lib.nix). It
# runs `command` with `env` added to the environment; the output of the
# program goes to the build log.

import json
import os
import subprocess


def main():
    with open(os.environ["NIX_ATTRS_JSON_FILE"]) as f:
        attrs = json.load(f)

    # A writable home directory, for tests that keep state (e.g. a store)
    # there. The values in `env` may refer to it as `$HOME`.
    os.environ["HOME"] = os.path.join(os.environ["TMPDIR"], "home")
    os.mkdir(os.environ["HOME"])
    env = dict(os.environ)
    for name, value in attrs["env"].items():
        env[name] = os.path.expandvars(value)

    subprocess.run(attrs["command"], env=env, check=True)
    os.mkdir(os.environ["out"])


main()
