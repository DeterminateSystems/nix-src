R"(

# Examples

* Show all active builds:

  ```console
  # nix ps
  /nix/store/125v7b7x0xaivgpprm72bv9a0zpg11af-boost-1.87.0.drv
  └───  1894008 bash -e /nix/store/vj1c3wf9c11a0qs6p3ymfvrnsdgsdcbq-source-stdenv.sh /nix/store/shkw4qm9qcw5sc5n1k5jznc83ny02r39-default-builder.sh
  ```

# Description

This command lists all currently running Nix builds.
For each build, it shows derivation path and the main process ID.
Depending on the platform, it may also show the full command line of the main build process.

)"
