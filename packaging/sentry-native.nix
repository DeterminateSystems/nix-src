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
  version = "0.17.4";

  src = fetchgit {
    url = "https://github.com/getsentry/sentry-native";
    tag = version;
    hash = "sha256-iVWIL/AM5X223HEDzyUFEyxqdOPD0/YU4Ycg9sAwfYE=";
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

  cmakeFlags = [
    # Link libcurl directly rather than possibly loading it at runtime,
    # so we use the curl from our closure.
    (lib.cmakeFeature "SENTRY_LINK_CURL" "ON")
  ];

  outputs = [
    "out"
    "dev"
  ];

  separateDebugInfo = true;
}
