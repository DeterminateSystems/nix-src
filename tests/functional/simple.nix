with import ./config.nix;

mkDerivation {
  name = "simple";
  builder = ./simple.builder.sh;
  _builder = ./simple.builder.sh;
  PATH = "";
  goodPath = path;
  meta = {
    position = "${__curPos.file}:${toString __curPos.line}";
    license = [
      # Since this file is from Nix, use Nix's license.
      # Keep in sync with `lib.licenses.lgpl21` from Nixpkgs.
      {
        deprecated = true;
        free = true;
        fullName = "GNU Lesser General Public License v2.1";
        redistributable = true;
        shortName = "lgpl21";
        spdxId = "LGPL-2.1";
        url = "https://spdx.org/licenses/LGPL-2.1.html";
      }
    ];
  };
}
