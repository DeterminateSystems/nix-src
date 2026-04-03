R""(

# Examples

* Verify the provenance of a store path:

  ```console
  # nix provenance verify /run/current-system
  ```

# Description

Verify the provenance of one or more store paths. This checks whether the store paths can be rebuilt from source. Specifically, it verifies the following:

* That source trees can be fetched.
* That flake evaluations result in the instantiation of the desired store paths (most commonly, store derivations).
* That derivations can be successfully rebuilt, producing identical outputs.

A non-zero exit code is returned if any of the verifications fail.

)""
