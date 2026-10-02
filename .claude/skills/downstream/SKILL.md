---
name: downstream
description: Downstreams an upstream Nix change (a commit rev or a NixOS/nix PR number) to Determinate Nix (DeterminateSystems/nix-src). Creates a branch and worktree from detsys/main, cherry-picks the commits, proposes upstream prerequisites if the change turns out to need them, builds and tests, pushes the branch and opens a PR against main. Use when the user asks to downstream, backport or cherry-pick an upstream Nix commit or PR into Determinate Nix.
argument-hint: <commit-rev | NixOS/nix PR number> [more revs/PRs...] [branch-name]
user-invocable: true
---

# Downstreaming an upstream Nix change to DeterminateSystems/nix-src

Arguments: `$ARGUMENTS`

Each argument is either a commit rev in upstream Nix, or a pull request number of https://github.com/NixOS/nix. Several may be given; they are cherry-picked in the order given. An argument that is neither a rev nor a number is used as the branch name.

Copy this checklist and check off items as you complete them:

```
- [ ] Verify and fetch the remotes
- [ ] Step 1: Determine the commits to cherry-pick
- [ ] Step 2: Create the branch and worktree
- [ ] Step 3: Cherry-pick, building and testing along the way
- [ ] Step 4: Format and run the full tests
- [ ] Step 5: Push and create the PR
```

## Remotes

* `origin` is upstream Nix, `git@github.com:NixOS/nix.git`. Its main branch is `master`.
* `detsys` is Determinate Nix, `git@github.com:DeterminateSystems/nix-src.git`. The new branch is pushed there and the PR is opened against its `main` branch.

Verify both with `git remote get-url` before doing anything. If `detsys` is missing, add it. Stop and tell the user if `origin` is not NixOS/nix.

Start by updating both: `git fetch origin master` and `git fetch detsys main`.

## Step 1: Determine the commits to cherry-pick

For a commit rev, that commit. For a PR, the PR's own commits (see `gh pr view N --repo NixOS/nix --json title,body,url,state,mergeCommit,commits`), without merge commits. Leave out commits that Determinate Nix already has.

## Step 2: Create the branch and worktree

Choose a short kebab-case branch name that describes the change, in the style of existing Determinate Nix branches such as `pin-fibers` or `wasm-no-suspend`. No `username/` prefix, no ticket number. If the user supplied a branch name, use it as is.

Create the worktree as a sibling of the main checkout, named `nix-<branch>`:

```
main=$(git worktree list --porcelain | head -n1 | cut -d' ' -f2-)
worktree=$(dirname "$main")/nix-<branch>
git worktree add --no-track -b <branch> "$worktree" detsys/main
echo "$worktree"
```

If the directory or branch already exists, pick a different name rather than reusing or deleting anything.

Shell variables do not persist between commands, so note the absolute path printed above and substitute it for `<worktree>` in every later command. Run every later command in that worktree, using `git -C <worktree>` for git and `cd <worktree> && ...` for everything else. Never touch the worktree the skill was started from.

## Step 3: Cherry-pick, building and testing along the way

```
git -C <worktree> cherry-pick -x <commits...>
```

Keep the original author, message and trailers. Do not add trailers of your own.

After each non-trivial cherry-pick (one that needed conflict resolution, or that is large or touches code Determinate Nix has changed), build and run the relevant tests before continuing, so that a problem is found at the commit that causes it. `AGENTS.md` in the worktree describes how to build and how to run individual tests; the `debug-fast` variant is enough here.

Resolve simple conflicts yourself, such as context drift, files that Determinate Nix renamed, or trivial API differences whose adaptation is obviously correct. Fix small build or test failures caused by a cherry-picked commit by amending that commit. Mention every such intervention in the final report.

For a bigger conflict or failure, first investigate whether the change depends on upstream commits that Determinate Nix does not have yet. If so, propose cherry-picking those prerequisites as well: tell the user which commits (and upstream PRs) they are and why the change needs them, and wait for the answer. If the problem is something else, for example Determinate Nix redesigned the same area, or you are unsure that a resolution preserves the intended behaviour, do not guess: leave things as they are, explain the problem and the options you see, and ask how to proceed.

## Step 4: Format and run the full tests

Run `nix develop -c ./maintainers/format.sh` in the worktree and fold any changes into the commits that introduced them. Then do the release build and run the full functional tests as described in `AGENTS.md`. This takes a long time; run it in the background and wait for it to finish.

Handle failures as in Step 3. A failure that also occurs on `detsys/main` is not caused by the change; mention it in the final report and carry on. Do not create the PR while the build or tests fail because of the change.

## Step 5: Push and create the PR

Write the PR description to a file in the scratchpad directory, starting from `.github/PULL_REQUEST_TEMPLATE.md` in the worktree:

* Motivation: reuse the upstream PR's own description, or summarise the commit messages if there is no PR. End with a link to the source, for example `Cherry-picked from https://github.com/NixOS/nix/pull/N.`
* Context: list any prerequisites that were included and why, and any adaptation that was needed for Determinate Nix. Leave the template's placeholder comments in place if there is nothing to say.
* Title: the upstream PR's title, or the commit's subject line.

Then:

```
git -C <worktree> push -u detsys <branch>:refs/heads/<branch>
gh pr create --repo DeterminateSystems/nix-src --base main --head <branch> --title "<title>" --body-file <body-file>
```

Finish with the final report: the PR URL, the branch name and worktree path, the cherry-picked commits (marking which were prerequisites), commits that were left out and why, conflicts and failures you fixed by hand, and the build and test outcome. Mention that the worktree can be removed later with `git worktree remove <worktree>`.
