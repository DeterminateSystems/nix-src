{
  lib,
  stdenv,
  fetchgit,
  cmake,
  curl,
  pkg-config,
  python3,
  darwin,
}:

stdenv.mkDerivation rec {
  pname = "sentry-native";
  version = "0.13.5";

  src = fetchgit {
    url = "https://github.com/getsentry/sentry-native";
    tag = version;
    hash = "sha256-vDBI6lB1DMLleAgRCfsHvTSdtmXOzvJSaNAt+NwOd3c=";
    fetchSubmodules = true;
  };

  dontFixCmake = true;

  nativeBuildInputs = [
    cmake
    pkg-config
  ]
  ++ lib.optionals stdenv.hostPlatform.isDarwin [
    python3
    darwin.bootstrap_cmds
  ];

  postPatch = ''
    # Borrowed from psutil: stick to the old SDK name for now.
    substituteInPlace external/crashpad/util/mac/mac_util.cc \
      --replace-fail kIOMainPortDefault kIOMasterPortDefault
  '';

  buildInputs = [
    curl
  ];

  cmakeBuildType = "RelWithDebInfo";

  cmakeFlags = [ ];

  outputs = [
    "out"
    "dev"
  ];
}
