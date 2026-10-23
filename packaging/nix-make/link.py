# The builder for linking a component's objects into a shared library or
# an executable. It reads the derivation's structured attributes (see
# `link` in lib.nix). The stdenv setup script has been sourced by the bash
# prelude in lib.nix, so the compiler driver (`$CXX`), the library paths
# of the build inputs and pkg-config are set up.

import json
import os
import subprocess
import sys


def main():
    with open(os.environ["NIX_ATTRS_JSON_FILE"]) as f:
        attrs = json.load(f)

    cxx = os.environ.get("CXX")
    if not cxx:
        sys.exit("CXX is not set; was the stdenv setup script sourced?")

    out = os.environ["out"]

    pkg_config_libs = []
    if attrs["linkPkgConfig"]:
        pkg_config_libs = subprocess.run(
            ["pkg-config", "--libs", *attrs["linkPkgConfig"]],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.split()

    common = ["-Wl,--as-needed", "-Wl,--no-undefined", *attrs["linkFlags"]]
    inputs = [*attrs["objects"], *pkg_config_libs, *attrs["linkLibs"]]

    if attrs["type"] == "library":
        lib = f"lib{attrs['libName']}.so"
        os.makedirs(f"{out}/lib")
        subprocess.run(
            [cxx, "-shared", "-fPIC", f"-Wl,-soname,{lib}", *common, "-o", f"{out}/lib/{lib}", *inputs],
            check=True,
        )
    elif attrs["type"] == "executable":
        os.makedirs(f"{out}/bin")
        subprocess.run([cxx, *common, "-o", f"{out}/bin/{attrs['exeName']}", *inputs], check=True)
        for name in attrs["binSymlinks"]:
            os.symlink(attrs["exeName"], f"{out}/bin/{name}")
    else:
        sys.exit(f"unknown component type: {attrs['type']}")

    # A Python snippet from the component description, with `out` bound.
    if attrs["postInstall"]:
        exec(attrs["postInstall"], {"out": out, "os": os})


main()
