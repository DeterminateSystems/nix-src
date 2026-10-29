# The builder for a compilation unit. It reads the derivation's structured
# attributes (see `compileUnit` in lib.nix) and runs the compiler once.
#
# This is Python rather than bash because the attributes carry typed JSON
# (e.g. `null` and booleans in `defines`), which bash cannot represent. The
# stdenv setup script has been sourced by the bash prelude in lib.nix, so
# the compiler (`$CXX`), the include paths of the build inputs and
# pkg-config are set up.

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

    # Recreate the source layout: each header and the source file is a
    # separate store path.
    os.mkdir("tree")
    os.chdir("tree")
    for path, store in attrs["includes"].items():
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        os.symlink(store, path)
    os.makedirs(os.path.dirname(attrs["srcPath"]) or ".", exist_ok=True)
    os.symlink(attrs["src"], attrs["srcPath"])

    include_flags = ["-I" + (d or ".") for d in attrs["includeDirs"]]

    # The configuration macros this unit is sensitive to: `null` means
    # undefined; strings become C string literals.
    define_flags = []
    for name, value in attrs["defines"].items():
        if value is None:
            define_flags.append(f"-U{name}")
        elif isinstance(value, bool):
            define_flags.append(f"-D{name}={int(value)}")
        elif isinstance(value, int):
            define_flags.append(f"-D{name}={value}")
        elif isinstance(value, str):
            define_flags.append(f"-D{name}={json.dumps(value)}")
        else:
            sys.exit(f"unsupported value for macro {name}: {value!r}")

    pkg_config_flags = []
    if attrs["pkgConfigDeps"]:
        pkg_config_flags = subprocess.run(
            ["pkg-config", "--cflags", *attrs["pkgConfigDeps"]],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.split()

    # Replace this process by the compiler rather than waiting for it.
    os.execvp(
        cxx,
        [
            cxx,
            *attrs["cxxFlags"],
            *include_flags,
            *define_flags,
            *pkg_config_flags,
            "-c",
            attrs["srcPath"],
            "-o",
            os.environ["out"],
        ],
    )


main()
