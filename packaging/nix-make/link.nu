# The builder for linking a component's objects into a shared library or
# an executable. It reads the derivation's structured attributes (see
# `link` in lib.nix). The stdenv setup script has been sourced by the bash
# prelude in lib.nix, so the compiler driver (`$CXX`), the library paths
# of the build inputs and pkg-config are set up.

def main [] {
  let attrs = open $env.NIX_ATTRS_JSON_FILE

  if ($env.CXX? | is-empty) {
    error make { msg: "CXX is not set; was the stdenv setup script sourced?" }
  }

  let pkg_config_libs = if ($attrs.linkPkgConfig | is-empty) {
    []
  } else {
    ^pkg-config --libs ...$attrs.linkPkgConfig | str trim | split row -r '\s+' | where { $in != "" }
  }

  match $attrs.type {
    "library" => {
      let lib = $"lib($attrs.libName).so"
      mkdir $"($env.out)/lib"
      (^$env.CXX -shared -fPIC
        $"-Wl,-soname,($lib)"
        -Wl,--as-needed -Wl,--no-undefined
        ...$attrs.linkFlags
        -o $"($env.out)/lib/($lib)"
        ...$attrs.objects
        ...$pkg_config_libs
        ...$attrs.linkLibs)
    }
    "executable" => {
      mkdir $"($env.out)/bin"
      (^$env.CXX
        -Wl,--as-needed -Wl,--no-undefined
        ...$attrs.linkFlags
        -o $"($env.out)/bin/($attrs.exeName)"
        ...$attrs.objects
        ...$pkg_config_libs
        ...$attrs.linkLibs)
      for name in $attrs.binSymlinks {
        ln -s $attrs.exeName $"($env.out)/bin/($name)"
      }
    }
    _ => { error make { msg: $"unknown component type: ($attrs.type)" } }
  }

  # A nushell snippet from the component description.
  if not ($attrs.postInstall | is-empty) {
    ^nu --no-config-file -c $attrs.postInstall
  }
}
