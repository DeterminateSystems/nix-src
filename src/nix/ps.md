R"(

# Examples

* Show all active builds:

  ```console
  # nix ps
  /nix/store/125v7b7x0xaivgpprm72bv9a0zpg11af-boost-1.87.0.drv
  └───2001892 bash -e /nix/store/vj1c3wf9c11a0qs6p3ymfvrnsdgsdcbq-source-stdenv.sh /
      └───2002786 b2 --includedir=/nix/store/c2r3lg107ab3h6y5x13snlm9vjsqzqaa-boost-1.87
          └───2002977 /bin/sh -c "g++" -o "bin.v2/standalone/ac/gcc-14/release/x86_64/thread
              └───2002978 /nix/store/8adzgnxs3s0pbj22qhk9zjxi1fqmz3xv-gcc-14.3.0/bin/g++ -fPIC -
                  └───2002983 /nix/store/8adzgnxs3s0pbj22qhk9zjxi1fqmz3xv-gcc-14.3.0/libexec/gcc/x86
                      └───2002984 /nix/store/cfqbabpc7xwg8akbcchqbq3cai6qq2vs-bash-5.2p37/bin/bash /nix/
                          └───2002988 /nix/store/ap35np2bkwaba3rxs3qlxpma57n2awyb-binutils-2.44/bin/ld -z re
  ```

# Description

This command lists all currently running Nix builds.
For each build, it shows derivation path and the main process ID.
Depending on the platform, it may also show the full command line of the main build process.

)"
