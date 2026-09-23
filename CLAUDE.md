Observe the following rules when contributing to this repository:

* Before committing, run ./maintainers/format.sh to detect/fix any formatting issues.

* Use "Assisted-by:" instead of "Co-Authored-By:" for the Claude trailer in commits.

* Do not create PRs unless prompted. Create PRs using `gh pr create --repo DeterminateSystems/nix-src --base main`, observing `.github/PULL_REQUEST_TEMPLATE.md`.

* The code base uses C++23, so C++23 features (e.g. deducing-this lambdas) can be used freely.

Building and testing Nix:

* Build Nix using `nix build ./packaging/nix-make#release.nix`. The result is in `./result`.

* Run the functional tests in `tests/functional` using `nix build ./packaging/nix-make#release.functional-tests`.

* Individual tests can be executed by doing `nix build ./packaging/nix-make#release.nix && nix run ./packaging/nix-make#test-runner -- --nix-bin-dir ./result/bin $TEST_NAMES`, where `TEST_NAMES` are the file names inside `tests/functional` (without the `.sh` suffix), e.g. `simple` or `flakes/relative-paths`.

* If you don't need an optimized build (e.g. you're not benchmarking), then instead of `release` you can use `debug`, e.g. `nix build ./packaging/nix-make#debug.nix`.
