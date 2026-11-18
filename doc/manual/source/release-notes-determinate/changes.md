# Changes between Nix and Determinate Nix

This section lists the differences between upstream Nix 2.35 and Determinate Nix 3.24.3.<!-- differences -->

* In Determinate Nix, flakes are stable. You no longer need to enable the `flakes` experimental feature.

* In Determinate Nix, the new Nix CLI (i.e. the `nix` command) is stable. You no longer need to enable the `nix-command` experimental feature.

* Determinate Nix has a setting [`json-log-path`](@docroot@/command-ref/conf-file.md#conf-json-log-path) to send a copy of all Nix log messages (in JSON format) to a file or Unix domain socket.

* Determinate Nix has made `nix profile install` an alias to `nix profile add`, a more symmetrical antonym of `nix profile remove`.

* `nix-channel` and `channel:` url syntax (like `channel:nixos-24.11`) is deprecated, see: https://github.com/DeterminateSystems/nix-src/issues/34

* Using indirect flake references and implicit inputs is deprecated, see: https://github.com/DeterminateSystems/nix-src/issues/37

* Warnings around "dirty trees" are updated to reduce "dirty" jargon, and now refers to "uncommitted changes".

<!-- Determinate Nix version 3.4.2 -->

<!-- Determinate Nix version 3.5.0 -->

<!-- Determinate Nix version 3.5.1 -->

* `nix upgrade-nix` is now inert, and suggests using `determinate-nixd upgrade`. [DeterminateSystems/nix-src#55](https://github.com/DeterminateSystems/nix-src/pull/55)

* Determinate Nix has Lazy Trees, avoiding expensive copying of flake inputs to the Nix store. ([DeterminateSystems/nix-src#27](https://github.com/DeterminateSystems/nix-src/pull/27), [DeterminateSystems/nix-src#56](https://github.com/DeterminateSystems/nix-src/pull/56))

<!-- Determinate Nix version 3.5.2 -->

<!-- Determinate Nix version 3.6.0 -->

<!-- Determinate Nix version 3.6.1 -->

<!-- Determinate Nix version 3.6.2 -->

* Documentation on how to replicate `nix-store --query --deriver` with the new `nix` cli. [DeterminateSystems/nix-src#82](https://github.com/DeterminateSystems/nix-src/pull/82)

* In `nix profile`, the symbols `ε` and `∅` have been replaced with descriptive English words. [DeterminateSystems/nix-src#81](https://github.com/DeterminateSystems/nix-src/pull/81)

<!-- Determinate Nix version 3.6.3 revoked -->

<!-- Determinate Nix version 3.6.4 revoked -->

<!-- Determinate Nix version 3.6.5 -->

* When remote building with `--keep-failed`, Determinate Nix shows "you can rerun" message if the derivation's platform is supported on this machine. [DeterminateSystems/nix-src#87](https://github.com/DeterminateSystems/nix-src/pull/87)

* Improved error message when `sandbox-paths` specifies a missing file. [DeterminateSystems/nix-src#88](https://github.com/DeterminateSystems/nix-src/pull/88)

<!-- Determinate Nix version 3.6.6 -->

<!-- Determinate Nix version 3.6.7 -->

<!-- Determinate Nix version 3.6.8 -->

<!-- Determinate Nix version 3.7.0 -->

* `nix store delete` now explains why deletion fails. [DeterminateSystems/nix-src#130](https://github.com/DeterminateSystems/nix-src/pull/130)

<!-- Determinate Nix version 3.8.0 -->

<!-- Determinate Nix version 3.8.1 -->

<!-- Determinate Nix version 3.8.2 -->

<!-- Determinate Nix version 3.8.3 -->

<!-- Determinate Nix version 3.8.4 -->

<!-- Determinate Nix version 3.8.5 -->

* Tab completing arguments to Nix avoids network access. [DeterminateSystems/nix-src#161](https://github.com/DeterminateSystems/nix-src/pull/161)

* Importing Nixpkgs and other tarballs to the cache is 2-4x faster. [DeterminateSystems/nix-src#149](https://github.com/DeterminateSystems/nix-src/pull/149)

* Adding paths to the store is significantly faster. [DeterminateSystems/nix-src#162](https://github.com/DeterminateSystems/nix-src/pull/162)

<!-- Determinate Nix version 3.8.6 -->

<!-- Determinate Nix version 3.9.0 -->

* Determinate Nix allows flake inputs to be fetched at build time. [DeterminateSystems/nix-src#49](https://github.com/DeterminateSystems/nix-src/pull/49)

<!-- Determinate Nix version 3.9.1 -->

* The default `nix flake init` template is much more useful. [DeterminateSystems/nix-src#180](https://github.com/DeterminateSystems/nix-src/pull/180)

<!-- Determinate Nix version 3.10.0 -->

<!-- Determinate Nix version 3.10.1 -->


<!-- Determinate Nix version 3.11.0 -->

* Determinate Nix can evaluate Nix expressions in parallel using multiple threads. This benefits commands like `nix flake show`, `nix flake check`, `nix search`, and `nix eval --json`. It also evaluates the dependencies of derivations in parallel. [DeterminateSystems/nix-src#125](https://github.com/DeterminateSystems/nix-src/pull/125), [DeterminateSystems/nix-src#572](https://github.com/DeterminateSystems/nix-src/pull/572)

<!-- Determinate Nix version 3.11.1 -->


<!-- Determinate Nix version 3.11.2 -->

* Determinate Nix only tries to substitute inputs if fetching from its original location fails.[DeterminateSystems/nix-src#202](https://github.com/DeterminateSystems/nix-src/pull/202)

<!-- Determinate Nix version 3.11.3 -->


<!-- Determinate Nix version 3.12.0 -->

* A new command `nix nario` that replaces `nix-store --export|--export`. It also has a new file format (`--format 2`) that supports store path attributes such as signatures, and that can be imported more efficiently. [DeterminateSystems/nix-src#215](https://github.com/DeterminateSystems/nix-src/pull/215)

* Determinate Nix prints the Nix version when using `-vv` or higher verbosity. [DeterminateSystems/nix-src#237](https://github.com/DeterminateSystems/nix-src/pull/237)


<!-- Determinate Nix version 3.12.1 -->

* During evaluation, you can read or import from the result of `builtins.fetchClosure`. [DeterminateSystems/nix-src#241](https://github.com/DeterminateSystems/nix-src/pull/241)

<!-- Determinate Nix version 3.12.2 -->

* Flakerefs in error messages and lockfile diffs are abbreviated for readability. [DeterminateSystems/nix-src#243](https://github.com/DeterminateSystems/nix-src/pull/243),  [DeterminateSystems/nix-src#264](https://github.com/DeterminateSystems/nix-src/pull/264)

<!-- Determinate Nix version 3.13.0 -->

<!-- Determinate Nix version 3.13.1 -->


<!-- Determinate Nix version 3.13.2 -->

* The Git fetcher doesn't compute `revCount` or `lastModified` if they're already specified [DeterminateSystems./nix-src#269](https://github.com/DeterminateSystems/nix-src/pull/269)

* The Git fetcher avoids doing a shallow Git fetch if it previously did a non-shallow fetch of the same repository. [DeterminateSystems/nix-src#270](https://github.com/DeterminateSystems/nix-src/pull/270)

* Determinate Nix has a builtin copy of the flake registry, making it more resilient to network outages. [DeterminateSystems/nix-src#271](https://github.com/DeterminateSystems/nix-src/pull/271)

<!-- Determinate Nix version 3.14.0 -->

* `nix build` and `nix profile` report failing or succeeding installables. [DeterminateSystems/nix-src#281](https://github.com/DeterminateSystems/nix-src/pull/281)

* `nix flake check` shows which outputs failed or succeeded. [DeterminateSystems/nix-src#285](https://github.com/DeterminateSystems/nix-src/pull/285)

* Determinate Nix has a `nix ps` command to show active builds, including their CPU time and memory usage. [DeterminateSystems/nix-src#282](https://github.com/DeterminateSystems/nix-src/pull/282), [DeterminateSystems/nix-src#715](https://github.com/DeterminateSystems/nix-src/pull/715)

* Determinate Nix has improved backward compatibility with lock files created by Nix < 2.20. [DeterminateSystems/nix-src#278](https://github.com/DeterminateSystems/nix-src/pull/278)

<!-- Determinate Nix version 3.15.0 -->

* Determinate Nix has a builtin function `builtins.filterAttrs`. [DeterminateSystems/nix-src#291](https://github.com/DeterminateSystems/nix-src/pull/291)

* `builtins.fetchTree` implicitly sets `__final = true` when a `narHash` is supplied. This allows the tree to be substituted. [DeterminateSystems/nix-src#297](https://github.com/DeterminateSystems/nix-src/pull/297)

<!-- Determinate Nix version 3.15.1 -->

<!-- Determinate Nix version 3.15.2 -->

<!-- Determinate Nix version 3.16.0 -->

* Determinate Nix has an experimental builtin `builtins.wasm` that allows the Nix language to be extended using any language that compiles to Wasm. [DeterminateSystems/nix-src#309](https://github.com/DeterminateSystems/nix-src/pull/309)

* `builtins.getFlake` supports path values. [DeterminateSystems/nix-src#338](https://github.com/DeterminateSystems/nix-src/pull/338)

* Determinate Nix has support for keeping track of the provenance of store paths. [DeterminateSystems/nix-src#321](https://github.com/DeterminateSystems/nix-src/pull/321)

<!-- Determinate Nix version 3.16.1 -->

<!-- Determinate Nix version 3.16.2 -->


<!-- Determinate Nix version 3.16.3 -->


<!-- Determinate Nix version 3.17.0 -->


<!-- Determinate Nix version 3.17.1 -->

<!-- Determinate Nix version 3.17.2 -->

<!-- Determinate Nix version 3.17.3 -->

<!-- Determinate Nix version 3.18.0 -->

* Determinate Nix can upload crash info to Sentry. [DeterminateSystems/nix-src#418](https://github.com/DeterminateSystems/nix-src/pull/418)

* Determinate Nix provides the pre-build hook with a JSON serialization of the derivation. [DeterminateSystems/nix-src#424](https://github.com/DeterminateSystems/nix-src/pull/424)

<!-- Determinate Nix version 3.18.1 -->


<!-- Determinate Nix version 3.19.0 -->

<!-- Determinate Nix version 3.19.1 -->

<!-- Determinate Nix version 3.20.0 -->

* Determinate Nix supports resuming from binary caches that don't support ranged requests. [DeterminateSystems/nix-src#445](https://github.com/DeterminateSystems/nix-src/pull/445)

* JSON log messages have a configurable session ID field. [DeterminateSystems/nix-src#440](https://github.com/DeterminateSystems/nix-src/pull/440)

<!-- Determinate Nix version 3.21.0 -->

* Determinate Nix has a command `nix serve` that allows any store to be served as a binary cache. [DeterminateSystems/nix-src#428](https://github.com/DeterminateSystems/nix-src/pull/428)

* Determinate Nix has experimental support for signing store paths using CNSA algorithms. [DeterminateSystems/nix-src#449](https://github.com/DeterminateSystems/nix-src/pull/449)

<!-- Determinate Nix version 3.21.1 -->

* Determinate Nix is built against Determinate Secure Packages. [DeterminateSystems/nix-src#288](https://github.com/DeterminateSystems/nix-src/pull/288)

<!-- Determinate Nix version 3.21.2 -->

* `nix optimise store` is  multi-threaded in Determinate Nix. [DeterminateSystems/nix-src#492](https://github.com/DeterminateSystems/nix-src/pull/492)

<!-- Determinate Nix version 3.21.3 -->

<!-- Determinate Nix version 3.21.4 -->

<!-- Determinate Nix version 3.21.5 -->

<!-- Determinate Nix version 3.21.6 -->

<!-- Determinate Nix version 3.21.7 -->

<!-- Determinate Nix version 3.21.8 -->

<!-- Determinate Nix version 3.21.9 -->

<!-- Determinate Nix version 3.22.0 -->

<!-- Determinate Nix version 3.22.1 -->

<!-- Determinate Nix version 3.22.2 -->

* `nix eval --json` has a flag `--drv-link` to create symlinks to top-level derivations. This prevents them from being garbage-collected. [DeterminateSystems/nix-src#599](https://github.com/DeterminateSystems/nix-src/pull/599)

<!-- Determinate Nix version 3.22.3 -->

<!-- Determinate Nix version 3.22.4 -->

<!-- Determinate Nix version 3.22.5 -->

<!-- Determinate Nix version 3.23.0 -->

* Determinate Nix has support for OpenTelemetry. [DeterminateSystems/nix-src#615](https://github.com/DeterminateSystems/nix-src/pull/615)

* Determinate Nix has an experimental command `nix flake bake` for creating pre-evaluated flakes from existing flakes. [DeterminateSystems/nix-src#450](https://github.com/DeterminateSystems/nix-src/pull/450)

<!-- Determinate Nix version 3.23.1 -->

* `nix profile history` reports packages whose store path changed but whose version stayed the same, and has a `--show-source` flag to show the flake reference of each package. [DeterminateSystems/nix-src#665](https://github.com/DeterminateSystems/nix-src/pull/665)

* Running out of memory makes Nix exit with status 12 rather than abort. [DeterminateSystems/nix-src#653](https://github.com/DeterminateSystems/nix-src/pull/653)

<!-- Determinate Nix version 3.23.2 -->

<!-- Determinate Nix version 3.23.3 -->

<!-- Determinate Nix version 3.24.0 -->

* Determinate Nix has a `nix top` command that shows a continuously updating view of the active builds and their CPU and memory usage. [DeterminateSystems/nix-src#710](https://github.com/DeterminateSystems/nix-src/pull/710)

<!-- Determinate Nix version 3.24.1 -->

<!-- Determinate Nix version 3.24.2 -->

* The cache of unpacked tarballs is a SQLite database (`~/.cache/nix/tarball-cache-detsys-v1.sqlite`) rather than a bare Git repository, which makes imports faster and more reliable under concurrency. [DeterminateSystems/nix-src#646](https://github.com/DeterminateSystems/nix-src/pull/646)

* The evaluator uses much less stack space, so deeply recursive Nix expressions can go roughly twice as deep before overflowing the stack. [DeterminateSystems/nix-src#752](https://github.com/DeterminateSystems/nix-src/pull/752)

<!-- Determinate Nix version 3.24.3 -->

* builtins.getFlake: Fix multi-component 'dir' in path flakerefs to store paths by @edolstra in [DeterminateSystems/nix-src#758](https://github.com/DeterminateSystems/nix-src/pull/758)

* Tarball cache: Use canonical URLs as cache keys by @edolstra in [DeterminateSystems/nix-src#760](https://github.com/DeterminateSystems/nix-src/pull/760)

* Check formatting in CI again by @edolstra in [DeterminateSystems/nix-src#759](https://github.com/DeterminateSystems/nix-src/pull/759)

* Avoid expensive store path parsing during evaluation by @edolstra in [DeterminateSystems/nix-src#762](https://github.com/DeterminateSystems/nix-src/pull/762)

* nix fmt: Throw an error if the flake has no local directory by @edolstra in [DeterminateSystems/nix-src#761](https://github.com/DeterminateSystems/nix-src/pull/761)

* nix serve: Write the port file atomically by @edolstra in [DeterminateSystems/nix-src#771](https://github.com/DeterminateSystems/nix-src/pull/771)

* Speed up the evaluator's allow-list checks by @edolstra in [DeterminateSystems/nix-src#763](https://github.com/DeterminateSystems/nix-src/pull/763)

* StructuredAttrs: Store the attributes as a nlohmann::json by @edolstra in [DeterminateSystems/nix-src#770](https://github.com/DeterminateSystems/nix-src/pull/770)

* Ignore SIGXFSZ by @edolstra in [DeterminateSystems/nix-src#774](https://github.com/DeterminateSystems/nix-src/pull/774)

* Count attribute selections towards max-call-depth by @edolstra in [DeterminateSystems/nix-src#775](https://github.com/DeterminateSystems/nix-src/pull/775)

* RemoteStore::queryPathInfos(): Release the connection before the callback by @jwiegley in [DeterminateSystems/nix-src#777](https://github.com/DeterminateSystems/nix-src/pull/777)

* Mercurial fetcher: Don't assert on unexpected 'hg log' output by @edolstra in [DeterminateSystems/nix-src#778](https://github.com/DeterminateSystems/nix-src/pull/778)

* Replace throws of std::bad_alloc by outOfMemory() in allocBytes<T, N>() by @edolstra in [DeterminateSystems/nix-src#779](https://github.com/DeterminateSystems/nix-src/pull/779)

* bdwgc: Reduce GC_MANY_BLOCKS_DEFAULT from 64 to 8 by @edolstra in [DeterminateSystems/nix-src#780](https://github.com/DeterminateSystems/nix-src/pull/780)

* fetchToStore(): Don't keep source accessors alive in the srcToStore cache by @edolstra in [DeterminateSystems/nix-src#781](https://github.com/DeterminateSystems/nix-src/pull/781)

* Crash handler: Make the cursor visible again by @edolstra in [DeterminateSystems/nix-src#783](https://github.com/DeterminateSystems/nix-src/pull/783)

* Fix static build by @edolstra in [DeterminateSystems/nix-src#784](https://github.com/DeterminateSystems/nix-src/pull/784)

* Don't keep the source accessors of path literals alive forever by @edolstra in [DeterminateSystems/nix-src#786](https://github.com/DeterminateSystems/nix-src/pull/786)

* Kill SSH master processes in quickExit() by @edolstra in [DeterminateSystems/nix-src#787](https://github.com/DeterminateSystems/nix-src/pull/787)

* Make EvalState extensible by @edolstra in [DeterminateSystems/nix-src#788](https://github.com/DeterminateSystems/nix-src/pull/788)

* test-runner: Add python3 to PATH by @edolstra in [DeterminateSystems/nix-src#790](https://github.com/DeterminateSystems/nix-src/pull/790)

* DerivationTrampolineGoal: Fix a race with the garbage collector by @edolstra in [DeterminateSystems/nix-src#791](https://github.com/DeterminateSystems/nix-src/pull/791)

* Sync with upstream 2.35.6 by @edolstra in [DeterminateSystems/nix-src#792](https://github.com/DeterminateSystems/nix-src/pull/792)

* Fix race with lastModifiedDate in fetchTree by @jmoo in [DeterminateSystems/nix-src#793](https://github.com/DeterminateSystems/nix-src/pull/793)

* Don't close the default substituters in quickExit() by @edolstra in [DeterminateSystems/nix-src#794](https://github.com/DeterminateSystems/nix-src/pull/794)

* Signal callbacks: Wait for running callbacks on deregistration by @edolstra in [DeterminateSystems/nix-src#795](https://github.com/DeterminateSystems/nix-src/pull/795)

* build.yml: Use the Nix that we just built by @edolstra in [DeterminateSystems/nix-src#796](https://github.com/DeterminateSystems/nix-src/pull/796)

* Don't count waiting for a connection towards connect-timeout by @edolstra in [DeterminateSystems/nix-src#789](https://github.com/DeterminateSystems/nix-src/pull/789)

* libutil: don't make ThreadPool work items wait for process() by @czawistowski-nvidia in [DeterminateSystems/nix-src#718](https://github.com/DeterminateSystems/nix-src/pull/718)

* RestoreSink: start writeback while restoring large files by @czawistowski-nvidia in [DeterminateSystems/nix-src#765](https://github.com/DeterminateSystems/nix-src/pull/765)

* Don't rethrow every exception that passes through a thunk by @edolstra in [DeterminateSystems/nix-src#800](https://github.com/DeterminateSystems/nix-src/pull/800)

* Throw an error for a text-hashed path that refers to itself by @edolstra in [DeterminateSystems/nix-src#799](https://github.com/DeterminateSystems/nix-src/pull/799)

* BinaryCacheStore: verify NarHash/NarSize before writing a narinfo by @czawistowski-nvidia in [DeterminateSystems/nix-src#766](https://github.com/DeterminateSystems/nix-src/pull/766)

* gc: don't hold the roots lock while marking a closure alive by @czawistowski-nvidia in [DeterminateSystems/nix-src#769](https://github.com/DeterminateSystems/nix-src/pull/769)

* Update Nixpkgs by @edolstra in [DeterminateSystems/nix-src#804](https://github.com/DeterminateSystems/nix-src/pull/804)

* collectGarbage(): Don't trust the path info cache by @edolstra in [DeterminateSystems/nix-src#806](https://github.com/DeterminateSystems/nix-src/pull/806)

* libutil: Skip root chmod on Darwin when deleting by @edolstra in [DeterminateSystems/nix-src#807](https://github.com/DeterminateSystems/nix-src/pull/807)

* collectGarbage(): Don't traverse shared closures for every root by @edolstra in [DeterminateSystems/nix-src#805](https://github.com/DeterminateSystems/nix-src/pull/805)

* Show garbage collector progress by @edolstra in [DeterminateSystems/nix-src#808](https://github.com/DeterminateSystems/nix-src/pull/808)

* libstore: don't delete anything in GC query modes by @edolstra in [DeterminateSystems/nix-src#809](https://github.com/DeterminateSystems/nix-src/pull/809)

* sentry.sh: Fix race when deleting the Sentry database by @edolstra in [DeterminateSystems/nix-src#810](https://github.com/DeterminateSystems/nix-src/pull/810)


* @jwiegley made their first contribution in [DeterminateSystems/nix-src#777](https://github.com/DeterminateSystems/nix-src/pull/777)

* @jmoo made their first contribution in [DeterminateSystems/nix-src#793](https://github.com/DeterminateSystems/nix-src/pull/793)
