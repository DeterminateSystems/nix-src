{
  lib,
  stdenv,
  fetchgit,
  fetchpatch,
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

  patches = [
    # Backport https://github.com/getsentry/crashpad/pull/171, included in
    # Sentry Native 0.17.2. Keep real filesystem errors visible while
    # treating an existing report lock as expected interprocess contention.
    (fetchpatch {
      name = "crashpad-quiet-report-lock-contention.patch";
      url = "https://github.com/getsentry/crashpad/commit/432de259e77091e26074ae6765dfc5a01d87ac12.patch";
      stripLen = 1;
      extraPrefix = "external/crashpad/";
      hash = "sha256-nnRL8DNeYf2ZRObtIV87elDFSNRN3J5ikH7gpZdVMQc=";
    })
  ];

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

  separateDebugInfo = true;
}
