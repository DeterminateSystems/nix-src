# The builder for a compilation unit. It reads the derivation's structured
# attributes (see `compileUnit` in lib.nix) and runs the compiler once.
#
# This is a nushell script rather than bash because the attributes carry
# typed JSON (e.g. `null` and booleans in `defines`), which bash cannot
# represent. The stdenv setup script has been sourced by the bash prelude
# in lib.nix, so the compiler (`$CXX`), the include paths of the build
# inputs and pkg-config are set up.

def main [] {
  let attrs = open $env.NIX_ATTRS_JSON_FILE

  if ($env.CXX? | is-empty) {
    error make { msg: "CXX is not set; was the stdenv setup script sourced?" }
  }

  # Recreate the source layout: each header and the source file is a
  # separate store path.
  mkdir tree
  cd tree
  for entry in ($attrs.includes | transpose path store) {
    mkdir ($entry.path | path dirname)
    ln -s $entry.store $entry.path
  }
  mkdir ($attrs.srcPath | path dirname)
  ln -s $attrs.src $attrs.srcPath

  let include_flags = $attrs.includeDirs | each {|dir|
    $"-I(if ($dir | is-empty) { '.' } else { $dir })"
  }

  # The configuration macros this unit is sensitive to: `null` means
  # undefined; strings become C string literals.
  let define_flags = $attrs.defines | transpose name value | each {|d|
    match ($d.value | describe) {
      "nothing" => $"-U($d.name)",
      "bool" => $"-D($d.name)=(if $d.value { 1 } else { 0 })",
      "int" => $"-D($d.name)=($d.value)",
      "string" => $"-D($d.name)=($d.value | to json)",
      _ => { error make { msg: $"unsupported value for macro ($d.name): ($d.value)" } },
    }
  }

  let pkg_config_flags = if ($attrs.pkgConfigDeps | is-empty) {
    []
  } else {
    ^pkg-config --cflags ...$attrs.pkgConfigDeps | str trim | split row -r '\s+' | where { $in != "" }
  }

  (exec $env.CXX
    ...$attrs.cxxFlags
    ...$include_flags
    ...$define_flags
    ...$pkg_config_flags
    -c $attrs.srcPath
    -o $env.out)
}
