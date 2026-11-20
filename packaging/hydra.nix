{
  inputs,
  forAllCrossSystems,
  forAllSystems,
  lib,
  linux64BitSystems,
  nixpkgsFor,
  nixComponentsFor,
  self,
}:
let
  inherit (inputs) nixpkgs nixpkgs-regression;

  installScriptFor =
    tarballs:
    nixpkgsFor.x86_64-linux.native.callPackage ./scripted-installer {
      inherit tarballs;
      # Platform doesn't matter, we only need to fish out the fineVersion.
      version = nixComponentsFor.x86_64-linux.native.nix-cli.version;
    };

  testNixVersions =
    components: daemon:
    components.nix-functional-tests.override {
      pname = "nix-daemon-compat-tests";
      version = "${components.nix-cli.version}-with-daemon-${daemon.version}";

      test-daemon = daemon;
    };

  # Technically we could just return `nixComponents`, but for Hydra it's
  # convention to transpose it, and to transpose it efficiently, we need to
  # enumerate them manually, so that we don't evaluate unnecessary package sets.
  # See listingIsComplete below.
  forAllPackages = forAllPackages' { };
  forAllPackages' =
    {
      enableDocs ? false, # already have separate attrs for these
    }:
    lib.genAttrs (
      [
        "nix-everything"
        "nix-util"
        "nix-util-c"
        "nix-util-test-support"
        "nix-util-tests"
        "nix-store"
        "nix-store-c"
        "nix-store-test-support"
        "nix-store-tests"
        "nix-fetchers"
        "nix-fetchers-c"
        "nix-fetchers-tests"
        "nix-expr"
        "nix-expr-c"
        "nix-expr-test-support"
        "nix-expr-tests"
        "nix-flake"
        "nix-flake-c"
        "nix-flake-tests"
        "nix-nswrapper"
        "nix-main"
        "nix-main-c"
        "nix-cmd"
        "nix-cli"
        "nix-functional-tests"
        "nix-json-schema-checks"
        "nix-clang-tidy-plugin"
      ]
      ++ lib.optionals enableDocs [
        "nix-manual"
        "nix-manual-manpages-only"
        "nix-internal-api-docs"
        "nix-external-api-docs"
      ]
    );
in
rec {
  /**
    An internal check to make sure our package listing is complete.
  */
  listingIsComplete =
    let
      arbitrarySystem = "x86_64-linux";
      listedPkgs = forAllPackages' {
        enableDocs = true;
      } (_: null);
      actualPkgs = lib.concatMapAttrs (
        k: v: if lib.strings.hasPrefix "nix-" k then { ${k} = null; } else { }
      ) nixComponentsFor.${arbitrarySystem}.native;
      diff = lib.concatStringsSep "\n" (
        lib.concatLists (
          lib.mapAttrsToList (
            k: _:
            if (listedPkgs ? ${k}) && !(actualPkgs ? ${k}) then
              [ "- ${k}: redundant?" ]
            else if !(listedPkgs ? ${k}) && (actualPkgs ? ${k}) then
              [ "- ${k}: missing?" ]
            else
              [ ]
          ) (listedPkgs // actualPkgs)
        )
      );
    in
    if listedPkgs == actualPkgs then
      { }
    else
      throw ''
        Please update the components list in hydra.nix (or fix this check)
        Differences:
        ${diff}
      '';

  # Binary package for various platforms.
  build = forAllPackages (
    pkgName:
    lib.filterAttrs (
      system: _do_not_touch:
      pkgName == "nix-nswrapper" -> nixpkgsFor.${system}.native.stdenv.hostPlatform.isLinux
    ) (forAllSystems (system: nixComponentsFor.${system}.native.${pkgName}))
  );

  shellInputs = removeAttrs (forAllSystems (
    system: self.devShells.${system}.default.inputDerivation
  )) [ "i686-linux" ];

  # Static analysis with clang-tidy
  clangTidy = lib.genAttrs linux64BitSystems (
    system:
    let
      tidyScope = nixComponentsFor.${system}.nativeForStdenv.clangStdenv.overrideScope (
        self: super: {
          withClangTidy = true;
          # clang-tidy doesn't seem to like unity builds.
          withUnityBuild = false;
          # nix-everything is built via callPackage (not the layer system), so
          # enableClangTidyLayer's doCheck=false doesn't reach it. Set it here
          # so checkInputs (the *-tests.tests.run derivations) aren't pulled in.
          nix-everything = super.nix-everything.overrideAttrs { doCheck = false; };
        }
      );
    in
    tidyScope.nix-everything
  );

  # Binary tarball for various platforms, containing a Nix store
  # with the closure of 'nix' package, and the second half of
  # the installation script.
  binaryTarball = forAllSystems (
    system: nixComponentsFor.${system}.native.callPackage ./scripted-installer/binary-tarball.nix { }
  );

  installerScriptForGHA = forAllSystems (
    system:
    nixpkgsFor.${system}.native.callPackage ./scripted-installer {
      tarballs = [ self.hydraJobs.binaryTarball.${system} ];
      # Platform doesn't matter, we only need to fish out the fineVersion.
      version = nixComponentsFor.x86_64-linux.native.nix-cli.version;
    }
  );

  /**
    `NixOS/nix-installer` with this revision's Nix closure embedded.
  */
  nixInstaller = lib.genAttrs (linux64BitSystems ++ [ "aarch64-darwin" ]) (
    system:
    let
      components = nixComponentsFor.${system}.native;
      pkgs = components._pkgs;
      # Embed the native (glibc) Nix even though the Linux installer
      # binary is static/musl.
      tarball = pkgs.callPackage ./nix-installer/tarball.nix {
        # Not nix-cli because we want manual pages too.
        nix = components.nix-everything;
      };
      builder = if pkgs.stdenv.hostPlatform.isLinux then pkgs.pkgsStatic else pkgs;
    in
    builder.callPackage ./nix-installer {
      inherit tarball;
    }
  );

  nixInstallerScript = nixpkgsFor.x86_64-linux.native.callPackage ./nix-installer/script.nix {
    nixInstallers = [
      # Native
      self.hydraJobs.nixInstaller."x86_64-linux"
      self.hydraJobs.nixInstaller."aarch64-linux"
      self.hydraJobs.nixInstaller."aarch64-darwin"
      # TODO: We don't do cross yet.
    ];

    # Platform doesn't matter, we only need to fish out the fineVersion.
    version = nixComponentsFor.x86_64-linux.native.nix-cli.version;
  };

  /**
    Docker image with Nix inside.
  */
  dockerImage = lib.genAttrs linux64BitSystems (
    system:
    let
      components = nixComponentsFor.${system}.native;
      pkgs = components._pkgs;
      image = pkgs.callPackage ../docker.nix {
        tag = components.nix-cli.version;
        # Override the nix used at build time to create the local store db. This is
        # the intended way to do this since https://github.com/NixOS/nixpkgs/pull/561007.
        # We are not doing cross builds yet, but splicing machinery should just work (tm)
        # if we do start.
        dockerTools = pkgs.dockerTools.override { nix = components.nix-cli; };
        nix = components.nix-cli;
      };
    in
    pkgs.runCommand "docker-image-tarball-${components.nix-cli.version}"
      { meta.description = "Docker image with Nix for ${system}"; }
      ''
        mkdir -p $out/nix-support
        image=$out/image.tar.gz
        ln -s ${image} $image
        echo "file binary-dist $image" >> $out/nix-support/hydra-build-products
      ''
  );

  # Line coverage analysis.
  coverage =
    (import ./../ci/gha/tests rec {
      withCoverage = true;
      pkgs = nixComponents._pkgs;
      nixComponents = nixComponentsFor.x86_64-linux.nativeForStdenv.clangStdenv;
      nixFlake = null;
      getStdenv = p: p.clangStdenv;
    }).codeCoverage.coverageReports.overrideAttrs
      {
        name = "nix-coverage"; # For historical consistency
      };

  /**
    Nix's manual
  */
  manual = nixComponentsFor.x86_64-linux.native.nix-manual;

  /**
    API docs for Nix's unstable internal C++ interfaces.
  */
  internal-api-docs = nixComponentsFor.x86_64-linux.native.nix-internal-api-docs;

  /**
    API docs for Nix's C bindings.
  */
  external-api-docs = nixComponentsFor.x86_64-linux.native.nix-external-api-docs;

  # System tests.
  tests =
    import ../tests/nixos rec {
      inherit lib nixpkgs;
      nixComponents = nixComponentsFor.x86_64-linux.native;
      pkgs = nixComponents._pkgs;
      inherit (self.inputs) nixpkgs-23-11;
    }
    // {

      # Make sure that nix-env still produces the exact same result
      # on a particular version of Nixpkgs.
      evalNixpkgs =
        let
          components = nixComponentsFor.x86_64-linux.native;
          inherit (components._pkgs) runCommand;
        in
        runCommand "eval-nixos" { buildInputs = [ components.nix-cli ]; } ''
          type -p nix-env
          # Note: we're filtering out nixos-install-tools because https://github.com/NixOS/nixpkgs/pull/153594#issuecomment-1020530593.
          (
            set -x
            time nix-env --store dummy:// -f ${nixpkgs-regression} -qaP --drv-path | sort | grep -v nixos-install-tools > packages
            [[ $(sha1sum < packages | cut -c1-40) = e01b031fc9785a572a38be6bc473957e3b6faad7 ]]
          )
          mkdir $out
        '';

      nixpkgsLibTests = forAllSystems (
        system:
        let
          components = nixComponentsFor.${system}.native;
          pkgs = components._pkgs;
        in
        import (nixpkgs + "/lib/tests/test-with-nix.nix") {
          inherit (pkgs) lib;
          inherit pkgs;
          nix = components.nix-cli;
        }
      );

      nixpkgsLibTestsLazy = forAllSystems (
        system:
        lib.overrideDerivation
          (import (nixpkgs + "/lib/tests/test-with-nix.nix") {
            lib = nixpkgsFor.${system}.native.lib;
            nix = self.packages.${system}.nix-cli;
            pkgs = nixpkgsFor.${system}.native;
          })
          (_: {
            "NIX_CONFIG" = "lazy-trees = true";
          })
      );

      filetransfer-retry-backoff = forAllSystems (
        system: nixComponentsFor.${system}.native.callPackage ../tests/filetransfer-retry-backoff { }
      );

      /**
        Run functional tests with against set of nix daemon versions to catch
        protocol incompatibilities.
      */
      daemonCompat = forAllSystems (
        system:
        let
          components = nixComponentsFor.${system}.native;
          pkgs = components._pkgs;
        in
        pkgs.runCommand "daemon-compat-tests" {
          againstSelf = testNixVersions components components.nix-cli;
          # Disabled because the latest stable version doesn't handle
          # `NIX_DAEMON_SOCKET_PATH` which is required for the tests to work
          # againstCurrentLatest = testNixVersions components pkgs.nixVersions.latest;
          # againstLatestStable = testNixVersions components pkgs.nixVersions.stable;
        } "touch $out"
      );
    };

  metrics.nixpkgs =
    let
      components = nixComponentsFor.x86_64-linux.native;
    in
    import "${nixpkgs-regression}/pkgs/top-level/metrics.nix" {
      nixpkgs = nixpkgs-regression;
      pkgs = components._pkgs // {
        nix = components.nix-cli;
      };
    };
}
