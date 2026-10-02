---
name: upstream
description: Upstreams a Determinate Nix change (a commit rev or a DeterminateSystems/nix-src PR number) to upstream Nix (NixOS/nix). Creates a branch and worktree from origin/master, cherry-picks the commits, builds and tests, drafts a PR description for the user to review, and only after approval pushes the branch and opens the PR. Use when the user asks to upstream, cherry-pick to upstream, or open an upstream PR for a Determinate Nix change.
argument-hint: <commit-rev | nix-src PR number> [more revs/PRs...] [branch-name]
user-invocable: true
---

# Upstreaming a Determinate Nix change to NixOS/nix

Arguments: `$ARGUMENTS`

Each argument is either a commit rev in Determinate Nix, or a pull request number of https://github.com/DeterminateSystems/nix-src. Several may be given; they are cherry-picked in the order given. An argument that is neither a rev nor a number is used as the branch name.

Copy this checklist and check off items as you complete them:

```
- [ ] Verify and fetch the remotes
- [ ] Step 1: Determine the commits to cherry-pick
- [ ] Step 2: Create the branch and worktree
- [ ] Step 3: Cherry-pick, building and testing along the way
- [ ] Step 4: Format and run the full tests
- [ ] Step 5: Draft the PR description and report
- [ ] STOP: wait for the user to approve the draft
- [ ] Step 6: Push and create the PR
```

## Remotes

* `origin` is upstream Nix, `git@github.com:NixOS/nix.git`. The new branch is pushed there and the PR is opened against its `master` branch.
* `detsys` is Determinate Nix, `git@github.com:DeterminateSystems/nix-src.git`. Its main branch is `main`.

Verify both with `git remote get-url` before doing anything. If `detsys` is missing, add it. Stop and tell the user if `origin` is not NixOS/nix.

Start by updating both: `git fetch origin master` and `git fetch detsys main`.

## Step 1: Determine the commits to cherry-pick

For a commit rev, that commit. For a PR, the PR's own commits (see `gh pr view N --repo DeterminateSystems/nix-src --json title,body,url,state,mergeCommit,commits`), without merge commits. Leave out commits that make no sense upstream, such as Determinate version bumps and Determinate release notes, and commits that upstream already has.

## Step 2: Create the branch and worktree

Choose a short kebab-case branch name that describes the change, in the style of existing upstream branches such as `fix-gc-interrupt-crash` or `progress-bar-signals`. No `username/` prefix, no ticket number. If the user supplied a branch name, use it as is.

Create the worktree as a sibling of the main checkout, named `nix-<branch>`:

```
main=$(git worktree list --porcelain | head -n1 | cut -d' ' -f2-)
worktree=$(dirname "$main")/nix-<branch>
git worktree add --no-track -b <branch> "$worktree" origin/master
echo "$worktree"
```

If the directory or branch already exists, pick a different name rather than reusing or deleting anything.

Shell variables do not persist between commands, so note the absolute path printed above and substitute it for `<worktree>` in every later command. Run every later command in that worktree, using `git -C <worktree>` for git and `cd <worktree> && ...` for everything else. Never touch the worktree the skill was started from.

## Step 3: Cherry-pick, building and testing along the way

```
git -C <worktree> cherry-pick <commits...>
```

Do not pass `-x`: Determinate commit hashes mean nothing in the upstream repository. Keep the original author, message and trailers, in particular any `Assisted-by:` trailers, which upstream requires for AI-assisted commits. Do not add trailers of your own.

After each non-trivial cherry-pick (one that needed conflict resolution, or that is large or touches code that differs upstream), build and run the relevant tests before continuing, so that a problem is found at the commit that causes it.

Resolve simple conflicts yourself, such as context drift, files that were renamed upstream, or trivial API differences whose adaptation is obviously correct. Drop hunks that touch Determinate-only files (`.version-determinate`, `doc/manual/source/release-notes-determinate/`); if the change deserves a release note upstream, add one under `doc/manual/rl-next/` instead. Fix small build or test failures caused by a cherry-picked commit by amending that commit. Mention every such intervention in the Step 5 report.

For a bigger conflict or failure, first investigate whether the change depends on Determinate Nix commits that were never upstreamed. If so, tell the user which commits (and nix-src PRs) they are and why the change needs them, propose upstreaming them as well or first, and wait for the answer. If the problem is something else, for example upstream redesigned the same area, or you are unsure that a resolution preserves the intended behaviour, do not guess: leave things as they are, explain the problem and the options you see, and ask how to proceed.

When done, check that nothing Determinate-specific is left in `git -C <worktree> diff origin/master..HEAD`.

## Step 4: Format and run the full tests

Run `nix develop -c ./maintainers/format.sh` in the worktree and fold any changes into the commits that introduced them. Then run `nix build -L .` in the worktree, which also runs the unit and functional tests. This takes a long time; run it in the background and wait for it to finish.

Handle failures as in Step 3. A failure that also occurs on `origin/master` is not caused by the change; mention it in the Step 5 report and carry on.

## Step 5: Draft the PR description, then stop for review

Write the PR description to a file in the scratchpad directory, starting from upstream's `.github/PULL_REQUEST_TEMPLATE.md` in the worktree:

* Put the proposed title on the first line as `Title: ...` so the user can edit it too: the Determinate PR's title, or the commit's subject line.
* Motivation: reuse the Determinate PR's own description, or summarise the commit messages if there is no PR. Drop anything that only applies to Determinate Nix or that upstream readers cannot see, such as internal ticket references. End with a link to the source, for example `Taken from https://github.com/DeterminateSystems/nix-src/pull/N.`
* Context: only fill it in when there is something to say. Leave the template's placeholder comments in place otherwise.
* Do not add any line attributing the PR to an agent or tool ("Generated with", "Co-Authored-By", "Assisted-by"). Upstream requires PR descriptions to be human-authored, which is why the user reviews and edits this draft before it is submitted.

Now report to the user and stop. The report must contain: the branch name and worktree path, the cherry-picked commits and any that were left out, conflicts and failures you fixed by hand, the build and test outcome, the path of the draft file, and the draft's contents. Ask the user to review or edit the file and to confirm that the PR may be created. Do not push and do not create the PR until they confirm.

## Step 6: After approval, push and create the PR

Re-read the draft file first, since the user may have edited it, and strip the `Title:` line from the body. Then:

```
git -C <worktree> push -u origin <branch>:refs/heads/<branch>
gh pr create --repo NixOS/nix --base master --head <branch> --title "<title>" --body-file <body-file>
```

Report the PR URL. Mention that the worktree can be removed later with `git worktree remove <worktree>`.
