R"(

# Examples

* Show all active builds:

  ```console
  # nix ps
  /nix/store/125v7b7x0xaivgpprm72bv9a0zpg11af-boost-1.87.0.drv
  ├───  1930681 bash -e /nix/store/vj1c3wf9c11a0qs6p3ymfvrnsdgsdcbq-source-stdenv.sh /nix/store/shkw4qm9qcw5sc5n1k5jznc83ny02r39-default-builder.sh
  ├───  1930689 tar xf /nix/store/s0ncgmqnrznz2yzha95f695vz88fd30y-boost_1_87_0.tar.bz2 --mode=+w --warning=no-timestamp
  └───  1930690 bzip2 -d
  ```

# Description

This command lists all currently running Nix builds.
For each build, it shows derivation path and the main process ID.
Depending on the platform, it may also show the full command line of the main build process.

)"
