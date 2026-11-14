---
name: release-notes
description: Edit the auto-generated Determinate Nix release notes for a release into shape. Use when asked to write, tweak, clean up or finish the release notes for a version (e.g. "write the release notes for 3.24.0"), or after the automated release process has committed "Generate release notes for <version>".
---

# Determinate Nix release notes

The automated release process commits "Generate release notes for <version>", which creates or updates two files in `doc/manual/source/release-notes-determinate/`:

* `v<version>.md`: the release notes for this version, as a flat GitHub-style "What's Changed" list of PR titles.
* `changes.md`: the cumulative list of differences between upstream Nix and Determinate Nix, with the new PRs appended verbatim under `<!-- Determinate Nix version <version> -->`.

Both need manual editing. Do not change any other file.

## Step 1: Gather information

1. Determine the version from `.version-determinate` (or from the user). Read `v<version>.md` and the tail of `changes.md`.
2. Read the two or three most recent `v*.md` files for style. `v3.23.0.md` (feature release) and `v3.23.1.md` (bug fix release) are good examples.
3. For every PR listed, read its description:
   ```
   gh pr view <N> --repo DeterminateSystems/nix-src --json title,body --jq '.title, .body'
   ```
   The "Motivation" section usually says what the user-visible effect is. Ignore the auto-generated "Summary by CodeRabbit" section. If the description is empty, look at the commit messages (`git log --format='%h %s%n%b' <merge-commit>^1..<merge-commit>^2`).
4. For "Sync with upstream" PRs, look at the merge commit's diff stat and the backported commits to summarize what user-visible fixes came in.

## Step 2: Classify each PR

* **Major**: new features, new commands/flags/settings, notable behavior changes, and significant performance improvements. Each gets its own `##` section. A bug fix only qualifies if the bug was a major issue for users in practice (widely reported, affecting common workflows); a fix for a crash that few users have hit yet is minor, even if the root cause was serious.
* **Minor**: bug fixes, test fixes, dependency/flake.lock updates, documentation, CI, refactoring. These are grouped into `##` sections like "Bug fixes", "Performance improvements", "Miscellaneous changes", with one concise line per PR.
* Closely related PRs (e.g. several PRs implementing one feature, or several GC crash fixes) can share one major section with `PRs: ...`.

## Step 3: Edit `v<version>.md`

Keep the header, the "Based on upstream Nix ..." line, the "New Contributors" section (if present) and the "**Full Changelog**" line as generated. Replace the "What's Changed" list with:

```markdown
## <Feature or fix title>

<One to three paragraphs in plain English: what changed, why it matters to the user, how to use it (with a `console` example if useful), and any caveats such as required experimental features or settings. If the change was backported from upstream, say so.>

PR: [DeterminateSystems/nix-src#NNN](https://github.com/DeterminateSystems/nix-src/pull/NNN)

## <Another major item>

...

PRs: [DeterminateSystems/nix-src#NNN](...), [DeterminateSystems/nix-src#MMM](...)

## Bug fixes

* <Readable description of the fix> by @author in [DeterminateSystems/nix-src#NNN](...)

## Performance improvements

* ... by @author in [DeterminateSystems/nix-src#NNN](...)

## Miscellaneous changes

* ... by @author in [DeterminateSystems/nix-src#NNN](...)
```

Guidelines:

* Major sections come first, most important first. Then the grouped minor sections, then "New Contributors" and the "Full Changelog" line.
* Section titles are short and user-facing, e.g. "`nix flake bake`", "OpenTelemetry Support", "Garbage collector crash fixes". Backticks for command names.
* Write for Nix users, not Nix developers: say what the user experiences ("`nix flake check` no longer crashes when ...") rather than the implementation ("Don't force cached eval errors inside a catch handler"). Mention internal details only when they help the user understand the scope (e.g. "only affects setups without a Nix daemon").
* The PR link lines are literally `PR: ` or `PRs: ` followed by comma-separated links.
* For a PR that cherry-picks an upstream PR, acknowledge the upstream author. Look up the author with `gh pr view <N> --repo NixOS/nix --json author --jq .author.login` and append to the PR line: `, cherry-picked from [NixOS/nix#<N>](https://github.com/NixOS/nix/pull/<N>) by @<author>`.
* In the minor-item lists, keep the generated `... by @author in [link]` format but reword cryptic PR titles into readable sentences. Drop internal prefixes like `libstore:` or function names unless they're the clearest description.
* Omit sections that would be empty.
* Link to manual pages where relevant using relative paths, e.g. `../language/derivation-metadata.md`.

## Step 4: Edit `changes.md`

`changes.md` lists only *differences relative to upstream Nix*: new features, new commands/flags/settings/builtins, and incompatible or deliberately different behavior. Under the new `<!-- Determinate Nix version <version> -->` marker:

* **Remove** bug fixes, performance improvements, crash fixes, test/CI/documentation/dependency changes, "Sync with upstream" PRs, and the "@user made their first contribution" line.
* **Remove** backports and cherry-picks from upstream Nix (e.g. PRs whose description says "Cherry-picked from https://github.com/NixOS/nix/pull/..."), even if they are new features, since they are not Determinate-specific.
* **Keep** the remaining items, rewritten as one-sentence present-tense feature descriptions followed by the PR link, matching the existing entries:
  ```markdown
  * Determinate Nix has a `nix foo` command for doing X. [DeterminateSystems/nix-src#NNN](https://github.com/DeterminateSystems/nix-src/pull/NNN)

  * `nix bar` has a flag `--baz` that does Y. [DeterminateSystems/nix-src#NNN](...)
  ```
  Items are separated by blank lines and have no `by @author in` text.
* It is normal for a bug fix release to leave the marker with nothing under it. Keep the marker.

Do not touch entries for earlier versions. The "This section lists the differences between upstream Nix ... and Determinate Nix <version>." line is updated by the release process, so leave it alone.

## Step 5: Finish

* Re-read both files once for consistency: every PR from the generated list appears exactly once in `v<version>.md`; links are well-formed; no "What's Changed" header remains.
* Don't commit unless asked. If asked, commit both files on the release branch with the message "Tweak release notes".
