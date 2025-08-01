with import ./config.nix;

mkDerivation {
  name = "gc-runtime";
  builder =
    # Test inline source file definitions.
    builtins.toFile "builder.sh" ''
      mkdir $out

      cat > $out/program <<EOF
      #! ${shell}
      echo x > \$TEST_ROOT/fifo
      sleep 10000
      EOF

      chmod +x $out/program
    '';
}
