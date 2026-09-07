{
  lib,
  config,
  ...
}:
let
  pkgs = config.nodes.machine.nixpkgs.pkgs;

  # Generate a fake root CA and a fake api.flakehub.com certificate.
  cert = pkgs.runCommand "cert" { nativeBuildInputs = [ pkgs.openssl ]; } ''
    mkdir -p $out

    openssl genrsa -out ca.key 2048
    openssl req -new -x509 -days 36500 -key ca.key \
      -subj "/C=NL/ST=Denial/L=Springfield/O=Dis/CN=Root CA" -out $out/ca.crt

    openssl req -newkey rsa:2048 -nodes -keyout $out/server.key \
      -subj "/C=CN/ST=Denial/L=Springfield/O=Dis/CN=api.flakehub.com" -out server.csr
    openssl x509 -req -extfile <(printf "subjectAltName=DNS:api.flakehub.com") \
      -days 36500 -in server.csr -CA $out/ca.crt -CAkey ca.key -CAcreateserial -out $out/server.crt
  '';

  # The prebuilt artifact that the fh-resolve input resolves to. It
  # must not have references.
  artifact = pkgs.runCommand "nix-wasm-rust-0.1.0" { } ''
    mkdir -p $out
    printf 'hello world' > $out/plugin.wasm
  '';

  # An artifact with references, which fh-resolve inputs must reject.
  artifactWithRefs = pkgs.runCommand "has-refs-0.1.0" { } ''
    mkdir -p $out
    echo ${artifact} > $out/ref
  '';

  # Static responses for the FlakeHub resolve endpoint
  # (/f/{org}/{project}/{version_req}/output/{attr_path}).
  api = pkgs.runCommand "flakehub-api" { } ''
    dir="$out/DeterminateSystems/nix-wasm-rust/^0/output"
    mkdir -p "$dir"
    cat > "$dir/packages.x86_64-linux.default" << EOF
    ${builtins.toJSON {
      attribute_path = "packages.x86_64-linux.default";
      store_path = "${artifact}";
      token = null;
    }}
    EOF

    dir="$out/DeterminateSystems/has-refs/^0/output"
    mkdir -p "$dir"
    cat > "$dir/packages.x86_64-linux.default" << EOF
    ${builtins.toJSON {
      attribute_path = "packages.x86_64-linux.default";
      store_path = "${artifactWithRefs}";
      token = null;
    }}
    EOF
  '';

  flake = pkgs.writeTextFile {
    name = "flake";
    destination = "/flake.nix";
    text = ''
      {
        inputs.artifact = {
          type = "fh-resolve";
          org = "DeterminateSystems";
          project = "nix-wasm-rust";
          version = "^0";
          output = "packages.x86_64-linux.default";
          flake = false;
        };

        outputs = { self, artifact }: {
          content = builtins.readFile (artifact + "/plugin.wasm");
        };
      }
    '';
  };
in

{
  name = "fh-resolve";

  nodes = {
    machine =
      { config, pkgs, ... }:
      {
        virtualisation.writableStore = true;
        virtualisation.additionalPaths = [
          artifact
          artifactWithRefs
        ];
        nix.settings.substituters = lib.mkForce [ ];
        networking.hosts."127.0.0.1" = [ "api.flakehub.com" ];
        security.pki.certificateFiles = [ "${cert}/ca.crt" ];

        services.httpd.enable = true;
        services.httpd.adminAddr = "foo@example.org";
        services.httpd.extraConfig = ''
          ErrorLog syslog:local6
        '';
        services.httpd.virtualHosts."api.flakehub.com" = {
          forceSSL = true;
          sslServerKey = "${cert}/server.key";
          sslServerCert = "${cert}/server.crt";
          servedDirs = [
            {
              urlPath = "/f";
              dir = api;
            }
          ];
        };
      };
  };

  testScript =
    { nodes }:
    ''
      # fmt: off
      import json

      start_all()

      machine.wait_for_unit("httpd.service")
      machine.wait_for_unit("multi-user.target")

      # Check that the fake resolve endpoint works.
      out = machine.succeed("curl --fail https://api.flakehub.com/f/DeterminateSystems/nix-wasm-rust/%5E0/output/packages.x86_64-linux.default")
      print(out)
      assert json.loads(out)["store_path"] == "${artifact}"

      # Lock a flake with an fh-resolve input.
      machine.succeed("cp -r ${flake} /tmp/flake && chmod -R u+w /tmp/flake")
      machine.succeed("nix flake lock /tmp/flake")
      lock = json.loads(machine.succeed("cat /tmp/flake/flake.lock"))
      locked = lock["nodes"]["artifact"]["locked"]
      print(locked)
      assert locked["type"] == "fh-resolve"
      assert locked["storePath"] == "${artifact}", "lock file does not record the resolved store path"
      nar_hash = locked["narHash"]

      # Evaluating the flake should yield the contents of the artifact.
      out = machine.succeed("nix eval --raw /tmp/flake#content")
      assert out == "hello world", f"unexpected artifact content: {out}"

      # Test the URL syntax.
      out = machine.succeed("""
        nix eval --impure --raw --expr '(builtins.fetchTree "fh-resolve:DeterminateSystems/nix-wasm-rust/%5E0#packages.x86_64-linux.default").narHash'
      """)
      assert out == nar_hash, f"unexpected NAR hash: {out}"

      # Fetching with an incorrect NAR hash should fail.
      out = machine.fail("""
        nix eval --impure --raw --expr '(builtins.fetchTree { type = "fh-resolve"; org = "DeterminateSystems"; project = "nix-wasm-rust"; version = "^0"; output = "packages.x86_64-linux.default"; narHash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="; }).narHash' 2>&1
      """)
      assert "NAR hash mismatch" in out, "NAR hash check did not fail with the expected error"

      # Store paths with references should be rejected.
      out = machine.fail("""
        nix eval --impure --raw --expr '(builtins.fetchTree { type = "fh-resolve"; org = "DeterminateSystems"; project = "has-refs"; version = "^0"; output = "packages.x86_64-linux.default"; }).narHash' 2>&1
      """)
      assert "has references" in out, "fetching a store path with references did not fail with the expected error"

      # Locked inputs should not require the API server.
      machine.succeed("systemctl stop httpd.service")
      machine.succeed("rm -rf /root/.cache/nix")
      out = machine.succeed("nix eval --raw /tmp/flake#content")
      assert out == "hello world", f"unexpected artifact content: {out}"
    '';
}
