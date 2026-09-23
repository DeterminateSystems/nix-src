# The functional test runner (run-tests.py) installed as `run-tests`, with
# the tools the tests need on its PATH. The `nix` to test is taken from
# PATH or given with `--nix-bin-dir`.
{
  lib,
  runCommand,
  makeWrapper,
  python3,
  bash,
  coreutils,
  jq,
  git,
  mercurial,
  unixtools,
  busybox-sandbox-shell,
  util-linux,
}:

runCommand "nix-test-runner"
  {
    nativeBuildInputs = [ makeWrapper ];
    meta.mainProgram = "run-tests";
  }
  ''
    install -D -m 755 ${./run-tests.py} $out/libexec/run-tests.py
    makeWrapper ${lib.getExe python3} $out/bin/run-tests \
      --add-flags $out/libexec/run-tests.py \
      --prefix PATH : ${
        lib.makeBinPath [
          bash
          coreutils
          jq
          git
          mercurial
          unixtools.script
          busybox-sandbox-shell
          util-linux
        ]
      }
  ''
