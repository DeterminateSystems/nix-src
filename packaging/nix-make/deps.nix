# Table mapping external (angle-bracket) `#include`s to packages, by
# prefix. More specific prefixes must come first. Each entry has:
#
# - `prefix`: matched against the start of the included path.
# - `pkg`: the package(s) providing the headers and libraries.
# - `pkgconfig`: pkg-config names used for compile and link flags.
# - `libs`: extra linker flags for libraries without pkg-config files.
{ pkgs }:

let
  # The overridden dependencies used by the Meson build, layered over Nixpkgs.
  deps = pkgs // pkgs.nixDependencies2;
in
[
  {
    prefix = "boost/context";
    pkg = deps.boost;
    libs = [ "-lboost_context" ];
  }
  {
    # Also matches coroutine2, which is header-only on top of libboost_context.
    prefix = "boost/coroutine";
    pkg = deps.boost;
    libs = [
      "-lboost_coroutine"
      "-lboost_context"
    ];
  }
  {
    prefix = "boost/thread";
    pkg = deps.boost;
    libs = [ "-lboost_thread" ];
  }
  {
    prefix = "boost/iostreams";
    pkg = deps.boost;
    libs = [ "-lboost_iostreams" ];
  }
  {
    prefix = "boost/url";
    pkg = deps.boost;
    libs = [ "-lboost_url" ];
  }
  {
    prefix = "boost/container";
    pkg = deps.boost;
    libs = [ "-lboost_container" ];
  }
  {
    prefix = "boost/";
    pkg = deps.boost;
  }
  {
    prefix = "nlohmann/";
    pkg = deps.nlohmann_json;
    pkgconfig = [ "nlohmann_json" ];
  }
  {
    prefix = "archive";
    pkg = deps.libarchive;
    pkgconfig = [ "libarchive" ];
  }
  {
    prefix = "openssl/";
    pkg = deps.openssl;
    pkgconfig = [ "libcrypto" ];
  }
  {
    prefix = "sodium.h";
    pkg = deps.libsodium;
    pkgconfig = [ "libsodium" ];
  }
  {
    prefix = "blake3.h";
    pkg = deps.libblake3;
    pkgconfig = [ "libblake3" ];
  }
  {
    prefix = "brotli/";
    pkg = deps.brotli;
    pkgconfig = [
      "libbrotlicommon"
      "libbrotlidec"
      "libbrotlienc"
    ];
  }
  {
    prefix = "zstd.h";
    pkg = deps.zstd;
    pkgconfig = [ "libzstd" ];
  }
  {
    prefix = "libcpuid/";
    pkg = deps.libcpuid;
    pkgconfig = [ "libcpuid" ];
  }
  {
    prefix = "curl/";
    pkg = deps.curl;
    pkgconfig = [ "libcurl" ];
  }
  {
    prefix = "seccomp.h";
    pkg = deps.libseccomp;
    pkgconfig = [ "libseccomp" ];
  }
  {
    prefix = "sqlite3.h";
    pkg = deps.sqlite;
    pkgconfig = [ "sqlite3" ];
  }
  {
    prefix = "gc.h";
    pkg = deps.boehmgc;
    pkgconfig = [ "bdw-gc" ];
  }
  {
    prefix = "gc/";
    pkg = deps.boehmgc;
    pkgconfig = [ "bdw-gc" ];
  }
  {
    # Header-only.
    prefix = "toml.hpp";
    pkg = deps.toml11;
  }
  {
    prefix = "toml11/";
    pkg = deps.toml11;
  }
  {
    prefix = "microhttpd.h";
    pkg = deps.libmicrohttpd;
    pkgconfig = [ "libmicrohttpd" ];
  }
  {
    # `<mimalloc.h>`, `<mimalloc-override.h>`, ...
    prefix = "mimalloc";
    pkg = deps.mimalloc;
    pkgconfig = [ "mimalloc" ];
  }
  {
    prefix = "lowdown.h";
    pkg = deps.lowdown;
    pkgconfig = [ "lowdown" ];
  }
  {
    prefix = "editline.h";
    pkg = deps.editline;
    pkgconfig = [ "libeditline" ];
  }
  {
    prefix = "git2/";
    pkg = deps.libgit2;
    pkgconfig = [ "libgit2" ];
  }
  {
    prefix = "wasmtime";
    pkg = [
      deps.wasmtime
      deps.wasmtime.lib
    ];
    libs = [ "-lwasmtime" ];
  }
]
