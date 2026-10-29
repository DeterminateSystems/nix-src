R"(

# Examples

* Show all active builds:

  ```console
  # nix ps
  USER      PID         CPU        RSS  DERIVATION/COMMAND
  nixbld11  3534394  110.2s  438.6 MiB  /nix/store/lzvdxlbr6xjd9w8py4nd2y2nnqb9gz7p-nix-util-tests-3.13.2.drv (wall=8s)
  nixbld11  3534394    0.8s    3.9 MiB  └───bash -e /nix/store/jwqf79v5p51x9mv8vx20fv9mzm2x7kig-source-stdenv.sh /nix/store/shkw4qm9qcw5sc5n1k
  nixbld11  3534751   36.3s    7.5 MiB      └───ninja -j24
  nixbld11  3535637    0.0s    4.1 MiB          ├───/nix/store/0v2jfvx71l1zn14l97pznvbqnhiq3pyd-gcc-14.3.0/bin/g++ -fPIC -fstack-clash-protect
  nixbld11  3535639    0.1s  412.3 MiB          │   └───/nix/store/0v2jfvx71l1zn14l97pznvbqnhiq3pyd-gcc-14.3.0/libexec/gcc/x86_64-unknown-linu
  nixbld11  3535658    0.0s    4.0 MiB          └───/nix/store/0v2jfvx71l1zn14l97pznvbqnhiq3pyd-gcc-14.3.0/bin/g++ -fPIC -fstack-clash-protect
  nixbld1   3534377    1.8s   16.2 MiB  /nix/store/nh2dx9cqcy9lw4d4rvd0dbsflwdsbzdy-patchelf-0.18.0.drv (wall=5s)
  nixbld1   3534377    1.8s    4.2 MiB  └───bash -e /nix/store/xk05lkk4ij6pc7anhdbr81appiqbcb01-default-builder.sh
  nixbld1   3535074    0.0s    3.8 MiB      └───/nix/store/21ymxxap3y8hb9ijcfah8ani9cjpv8m6-bash-5.2p37/bin/bash ./configure --disable-depende
  ```

# Description

This command lists all currently running Nix builds.
For each build, it shows the derivation path and the main process ID.
On Linux and macOS, it also shows the child processes of each build.

The `CPU` column shows the CPU time used, and the `RSS` column shows the resident memory (resident set size).
For a build, the memory is taken from the build's cgroup if available (on Linux with cgroups enabled).
Otherwise it's the sum of the memory of its processes, which overestimates memory usage because memory shared between processes is counted multiple times.

)"
