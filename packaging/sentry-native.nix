{
  lib,
  stdenv,
  fetchgit,
  cmake,
  curl,
  pkg-config,
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

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

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
