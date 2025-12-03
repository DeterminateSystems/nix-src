R"(

# Examples

* Show all active builds:

  ```console
  # nix ps
  USER      PID      CPU  DERIVATION/COMMAND
  nixbld10  3340496  1s   /nix/store/96q6n4q5w0sqdjx3694dd3kpy5xkc2l6-determinate-nix-store-3.13.2.drv (wall=1s)
  nixbld10  3340496  0s   └───bash -e /nix/store/vj1c3wf9c11a0qs6p3ymfvrnsdgsdcbq-source-stdenv.sh /nix/store/shkw4qm9qcw5sc5n1k5jznc83ny02r39-default-builder.sh
  nixbld10  3340610  0s       └───/nix/store/vxl8pzgkkw8vdb4agzwm58imrfclmfrx-python3-3.12.11/bin/python3.12 /nix/store/dg3l1ny5a0xpfnmqv5h8fyvqdwazb76v-meson-1.
  nixbld1   3340491  0s   /nix/store/nh2dx9cqcy9lw4d4rvd0dbsflwdsbzdy-patchelf-0.18.0.drv (wall=1s)
  nixbld1   3340491  0s   └───bash -e /nix/store/v6x3cs394jgqfbi0a42pam708flxaphh-default-builder.sh
  nixbld1   3340525  0s       └───/nix/store/3vq9qasxlqpyq1k95nq3s13g2m6w59ay-perl-5.40.0/bin/perl /nix/store/wky99hgy2w2nb3a6hzgk96yqdbi95zsj-autoconf-2.72/bin/
  nixbld1   3340965  0s           └───/nix/store/3vq9qasxlqpyq1k95nq3s13g2m6w59ay-perl-5.40.0/bin/perl /nix/store/wky99hgy2w2nb3a6hzgk96yqdbi95zsj-autoconf-2.72/
  ```

# Description

This command lists all currently running Nix builds.
For each build, it shows derivation path and the main process ID.
Depending on the platform, it may also show the full command line of the main build process.

)"
