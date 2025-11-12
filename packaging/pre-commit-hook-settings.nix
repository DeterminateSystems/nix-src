{
  pkgs,
  lib,
  src,
}:

{
  inherit src;

  hooks = {
    check-merge-conflicts.enable = true;
    check-merge-conflicts-2 = {
      enable = true;
      entry = "${pkgs.writeScript "check-merge-conflicts" ''
        #!${pkgs.runtimeShell}
        conflicts=false
        for file in "$@"; do
          if grep --with-filename --line-number -E '^>>>>>>> ' -- "$file"; then
            conflicts=true
          fi
        done
        if $conflicts; then
          echo "ERROR: found merge/patch conflicts in files"
          exit 1
        fi
      ''}";
    };
    meson-format =
      let
        meson = pkgs.meson.overrideAttrs {
          doCheck = false;
          doInstallCheck = false;
          patches = [
            (pkgs.fetchpatch {
              url = "https://github.com/mesonbuild/meson/commit/38d29b4dd19698d5cad7b599add2a69b243fd88a.patch";
              hash = "sha256-PgPBvGtCISKn1qQQhzBW5XfknUe91i5XGGBcaUK4yeE=";
            })
          ];
        };
      in
      {
        enable = true;
        files = "(meson.build|meson.options)$";
        entry = "${pkgs.writeScript "format-meson" ''
          #!${pkgs.runtimeShell}
          for file in "$@"; do
            ${lib.getExe meson} format -ic ${../meson.format} "$file"
          done
        ''}";
      };
    nixfmt-rfc-style = {
      enable = true;
      excludes = [
        ''^tests/functional/lang/parse-.*\.nix$''
        ''^tests/functional/lang/eval-okay-curpos\.nix$''
        ''^tests/functional/lang/.*comment.*\.nix$''
        ''^tests/functional/lang/.*newline.*\.nix$''
        ''^tests/functional/lang/.*eol.*\.nix$''
        ''^tests/functional/shell.shebang\.nix$''
        ''^tests/functional/lang/eval-okay-ind-string\.nix$''
        ''^tests/functional/lang/eval-okay-deprecate-cursed-or\.nix$''
        ''^tests/functional/lang/eval-okay-attrs5\.nix$''
        ''^tests/functional/lang/eval-fail-eol-2\.nix$''
        ''^tests/functional/lang/eval-fail-path-slash\.nix$''
        ''^tests/functional/lang/eval-fail-toJSON-non-utf-8\.nix$''
        ''^tests/functional/lang/eval-fail-set\.nix$''
      ];
    };
    clang-format = {
      enable = true;
      package = pkgs.llvmPackages_latest.clang-tools;
      excludes = [
        ''^src/[^/]*-tests/data/.*$''
        ''^doc/manual/redirects\.js$''
        ''^doc/manual/theme/highlight\.js$''
      ];
    };
    shellcheck.enable = true;
  };
}
