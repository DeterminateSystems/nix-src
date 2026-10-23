R"(

# Examples

* Show the active builds:

  ```console
  # nix top
  nix top - 14:02:17  2 builds, 6 processes, 812.4% CPU, 454.8 MiB RSS
  USER          PID   %CPU    TIME        RSS  WALL  DERIVATION/COMMAND
  nixbld11  3534394  790.3  110.2s  438.6 MiB   48s  /nix/store/lzvdxlbr6xjd9w8py4nd2y2nnqb9gz7p-nix-util-tests-3.13.2.drv
  nixbld11  3534394    0.0    0.8s    3.9 MiB        └───bash -e /nix/store/jwqf79…-source-stdenv.sh
  nixbld11  3534751    1.5   36.3s    7.5 MiB            └───ninja -j24
  nixbld11  3535637   98.0    4.1s  208.1 MiB                ├───g++ -fPIC
  nixbld11  3535658   99.5    3.0s  215.0 MiB                └───g++ -fPIC
  nixbld1   3534377   22.1    1.8s   16.2 MiB    5s  /nix/store/nh2dx9cqcy9lw4d4rvd0dbsflwdsbzdy-patchelf-0.18.0.drv
  nixbld1   3534377    0.0    1.8s    4.2 MiB        └───bash -e /nix/store/xk05lk…-default-builder.sh
  ```

# Description

This command shows the currently running Nix builds in a continuously updating full-screen view, similar to
`top`. It shows the same information as [`nix ps`](./nix3-ps.md), plus the CPU usage of each build and
process since the previous update.

The columns are:

* `USER`: The user that the process runs as.
* `PID`: The process ID.
* `%CPU`: CPU usage since the previous update. 100% corresponds to one fully used CPU core.
* `TIME`: Total CPU time consumed. For a process, this includes the CPU time of its reaped children.
* `RSS`: Resident memory. For a build, this is taken from its cgroup if available (counting shared pages once);
  otherwise it's the sum of the RSS of its processes.
* `WALL`: How long the build has been running.
* `DERIVATION/COMMAND`: The derivation being built, or the command line of a process in the build.

The following keys are supported:

* `q`, `Ctrl-C`: Quit.
* `↑`/`↓`, `k`/`j`: Scroll one line.
* `PgUp`/`PgDn`: Scroll one page.
* `Home`/`End`, `g`/`G`: Scroll to the top or bottom.
* `t`, `Enter`: Toggle between showing the processes of each build and showing only the builds.
* `c`: Toggle compact mode (enabled by default). This shows only the program name rather than the full path of
  the executable of each process (e.g. `g++` instead of `/nix/store/…/bin/g++`), and abbreviates the hash parts of
  other store paths in command lines (e.g. `/nix/store/2limljgc50jy76vg388ldgggcjhhkhav-gcc-15.2.0` is shown as
  `/nix/store/2limlj…-gcc-15.2.0`).
* `s`: Cycle the sort order of builds: by CPU usage, by RSS, by wall time, or by name.
* `+`/`-`: Increase or decrease the refresh interval by 0.5 seconds (default 2 seconds).
* `Space`: Refresh now.
* `Ctrl-L`: Redraw the screen.

)"
