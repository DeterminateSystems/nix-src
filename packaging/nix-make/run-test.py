# The builder for running a test program (see `runTest` in lib.nix). It
# runs `command` with `env` added to the environment; the output of the
# program goes to the build log.

import json
import os
import subprocess


def main():
    with open(os.environ["NIX_ATTRS_JSON_FILE"]) as f:
        attrs = json.load(f)

    subprocess.run(attrs["command"], env={**os.environ, **attrs["env"]}, check=True)
    os.mkdir(os.environ["out"])


main()
