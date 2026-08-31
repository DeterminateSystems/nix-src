{
  lib,
  config,
  nixpkgs,
  ...
}:

let
  pkgs = config.nodes.machine.nixpkgs.pkgs;

  # A resource that is not a tarball. An input that points at it is a
  # `file` input, not a `tarball` input.
  bom = pkgs.writeText "bom.json" ''
    {"hello": "world"}
  '';

  # A flake with a `flake = false` input, to show that the Lockable HTTP
  # Protocol applies to `file` inputs.
  mkFileFlake =
    name: url:
    pkgs.writeTextFile {
      inherit name;
      destination = "/flake.nix";
      text = ''
        {
          inputs.foo = {
            url = "${url}";
            flake = false;
          };
          outputs = { self, foo }: { };
        }
      '';
    };

  fileFlake = mkFileFlake "file-flake" "http://localhost/file/stable/aarch64-linux?a=1";
  badHashFlake = mkFileFlake "bad-hash-flake" "http://localhost/file-bad-hash/stable/aarch64-linux";
  badTypeFlake = mkFileFlake "bad-type-flake" "http://localhost/file-bad-type/stable/aarch64-linux";

  root = pkgs.runCommand "nixpkgs-flake" { nativeBuildInputs = [ pkgs.nix ]; } ''
    mkdir -p $out/{stable,tags}

    set -x
    dir=nixpkgs-${nixpkgs.shortRev}
    cp -rd --preserve=ownership,timestamps ${nixpkgs} $dir
    # Set the correct timestamp in the tarball.
    find $dir -print0 | xargs -0 touch -h -t ${builtins.substring 0 12 nixpkgs.lastModifiedDate}.${
      builtins.substring 12 2 nixpkgs.lastModifiedDate
    } --
    tar cfz $out/stable/${nixpkgs.rev}.tar.gz $dir --hard-dereference

    # Set the "Link" header on the redirect but not the final response to
    # simulate an S3-like serving environment where the final host cannot set
    # arbitrary headers.
    cat >$out/tags/.htaccess <<EOF
    Redirect "/tags/latest.tar.gz" "/stable/${nixpkgs.rev}.tar.gz"
    Header always set Link "<http://localhost/stable/${nixpkgs.rev}.tar.gz?rev=${nixpkgs.rev}&revCount=1234>; rel=\"immutable\""
    EOF

    # A non-tarball resource, served at a URL that has no tarball
    # extension. Nix fetches it as a `file` input.
    mkdir -p $out/file/{stable,v1} $out/file-bad-hash/stable $out/file-bad-type/stable
    for d in file/stable file/v1 file-bad-hash/stable file-bad-type/stable; do
      cp ${bom} $out/$d/aarch64-linux
    done

    # The NAR hash of the resource, percent-encoded for a URL query.
    narHash=$(nix-hash --to-sri --type sha256 "$(nix-hash --type sha256 $out/file/v1/aarch64-linux)" \
      | sed 's|+|%2B|g; s|/|%2F|g; s|=|%3D|g')

    cat >$out/file/stable/.htaccess <<EOF
    Header always set Link "<http://localhost/file/v1/aarch64-linux?narHash=$narHash>; rel=\"immutable\""
    EOF

    # An immutable URL with a NAR hash that does not agree with the
    # contents.
    cat >$out/file-bad-hash/stable/.htaccess <<EOF
    Header always set Link "<http://localhost/file/v1/aarch64-linux?narHash=sha256-tbudgBSg%2BbHWHiHnlteNzN8TUvI80ygS9IULh4rklEw%3D>; rel=\"immutable\""
    EOF

    # An immutable URL that is a tarball flakeref, not a file flakeref.
    cat >$out/file-bad-type/stable/.htaccess <<EOF
    Header always set Link "<http://localhost/stable/${nixpkgs.rev}.tar.gz>; rel=\"immutable\""
    EOF
  '';
in

{
  name = "tarball-flakes";

  nodes = {
    machine =
      { config, pkgs, ... }:
      {
        networking.firewall.allowedTCPPorts = [ 80 ];

        services.httpd.enable = true;
        services.httpd.adminAddr = "foo@example.org";
        services.httpd.extraConfig = ''
          ErrorLog syslog:local6
        '';
        services.httpd.virtualHosts."localhost" = {
          servedDirs = [
            {
              urlPath = "/";
              dir = root;
            }
          ];
        };

        virtualisation.writableStore = true;
        virtualisation.diskSize = 2048;
        virtualisation.additionalPaths = [
          pkgs.hello
          pkgs.fuse
          fileFlake
          badHashFlake
          badTypeFlake
        ];
        virtualisation.memorySize = 4096;
        nix.settings.substituters = lib.mkForce [ ];
      };
  };

  testScript =
    { nodes }:
    ''
      # fmt: off
      import json

      start_all()

      machine.wait_for_unit("httpd.service")

      out = machine.succeed("nix flake metadata --json http://localhost/tags/latest.tar.gz")
      print(out)
      info = json.loads(out)

      # Check that we got redirected to the immutable URL.
      assert info["locked"]["url"] == "http://localhost/stable/${nixpkgs.rev}.tar.gz"

      # Check that we got a fingerprint for caching.
      assert info["fingerprint"]

      # Check that we got the rev and revCount attributes.
      assert info["revision"] == "${nixpkgs.rev}"
      assert info["revCount"] == 1234

      # Check that a 0-byte HTTP 304 "Not modified" result works.
      machine.succeed("nix flake metadata --refresh --json http://localhost/tags/latest.tar.gz")

      # Check that fetching with rev/revCount/narHash succeeds.
      machine.succeed("nix flake metadata --json http://localhost/tags/latest.tar.gz?rev=" + info["revision"])
      machine.succeed("nix flake metadata --json http://localhost/tags/latest.tar.gz?revCount=" + str(info["revCount"]))
      machine.succeed("nix flake metadata --json http://localhost/tags/latest.tar.gz?narHash=" + info["locked"]["narHash"])

      # Check that fetching fails if we provide incorrect attributes.
      machine.fail("nix flake metadata --json http://localhost/tags/latest.tar.gz?rev=493300eb13ae6fb387fbd47bf54a85915acc31c0")
      machine.fail("nix flake metadata --json http://localhost/tags/latest.tar.gz?narHash=sha256-tbudgBSg+bHWHiHnlteNzN8TUvI80ygS9IULh4rklEw=")

      # The protocol also applies to `file` inputs, that is, inputs with
      # `flake = false` whose URL has no tarball extension.
      machine.succeed("cp -rT ${fileFlake} /tmp/file-flake && chmod -R u+w /tmp/file-flake")
      machine.succeed("nix flake lock /tmp/file-flake")
      node = json.loads(machine.succeed("cat /tmp/file-flake/flake.lock"))["nodes"]["foo"]
      print(node)

      # Check that the lock file records the immutable URL, and that the
      # input is still a file.
      assert node["locked"]["url"] == "http://localhost/file/v1/aarch64-linux"
      assert node["locked"]["type"] == "file"
      assert node["original"]["url"] == "http://localhost/file/stable/aarch64-linux?a=1"

      # Check that Nix compares the contents against the `narHash` in the
      # immutable URL.
      machine.succeed("cp -rT ${badHashFlake} /tmp/bad-hash-flake && chmod -R u+w /tmp/bad-hash-flake")
      out = machine.fail("nix flake lock /tmp/bad-hash-flake 2>&1")
      print(out)
      assert "NAR hash mismatch in the immutable URL" in out

      # Check that Nix rejects an immutable URL of a different input type.
      machine.succeed("cp -rT ${badTypeFlake} /tmp/bad-type-flake && chmod -R u+w /tmp/bad-type-flake")
      out = machine.fail("nix flake lock /tmp/bad-type-flake 2>&1")
      print(out)
      assert "is a 'tarball' input, but a 'file' input is necessary" in out
    '';

}
