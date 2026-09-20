# Table mapping external (angle-bracket) `#include`s to packages, by
# prefix. More specific prefixes must come first. Each entry has:
#
# - `prefix`: matched against the start of the included path.
# - `pkg`: the package(s) providing the headers and libraries.
# - `pkgconfig`: pkg-config names used for compile and link flags.
# - `libs`: extra linker flags for libraries without pkg-config files.
# - `cflags`: extra compiler flags for libraries without pkg-config files.
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
    # The AWS CRT and its C libraries have no pkg-config files; the
    # defines are what their CMake targets would set.
    prefix = "aws/";
    pkg = with deps; [
      aws-crt-cpp
      aws-c-auth
      aws-c-cal
      aws-c-common
      aws-c-compression
      aws-c-event-stream
      aws-checksums
      aws-c-http
      aws-c-io
      aws-c-mqtt
      aws-c-s3
      aws-c-sdkutils
      s2n-tls
    ];
    cflags = [
      "-DAWS_ENABLE_EPOLL"
      "-DAWS_AUTH_USE_IMPORT_EXPORT"
      "-DAWS_CAL_USE_IMPORT_EXPORT"
      "-DAWS_CHECKSUMS_USE_IMPORT_EXPORT"
      "-DAWS_COMMON_USE_IMPORT_EXPORT"
      "-DAWS_COMPRESSION_USE_IMPORT_EXPORT"
      "-DAWS_CRT_CPP_USE_IMPORT_EXPORT"
      "-DAWS_EVENT_STREAM_USE_IMPORT_EXPORT"
      "-DAWS_HTTP_USE_IMPORT_EXPORT"
      "-DAWS_IO_USE_IMPORT_EXPORT"
      "-DAWS_MQTT_USE_IMPORT_EXPORT"
      "-DAWS_S3_USE_IMPORT_EXPORT"
      "-DAWS_SDKUTILS_USE_IMPORT_EXPORT"
    ];
    libs = [
      "-laws-crt-cpp"
      "-laws-c-auth"
      "-laws-c-cal"
      "-laws-c-common"
      "-laws-c-compression"
      "-laws-c-event-stream"
      "-laws-checksums"
      "-laws-c-http"
      "-laws-c-io"
      "-laws-c-mqtt"
      "-laws-c-s3"
      "-laws-c-sdkutils"
      "-ls2n"
    ];
  }
  {
    prefix = "opentelemetry/";
    pkg = deps.opentelemetry-cpp;
    pkgconfig = [ "opentelemetry_trace" ];
  }
  {
    # No pkg-config file.
    prefix = "sentry.h";
    pkg = deps.sentry-native;
    libs = [ "-lsentry" ];
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
