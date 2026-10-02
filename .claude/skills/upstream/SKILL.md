---
name: upstream
description: Upstreams a Determinate Nix change (a commit rev or a DeterminateSystems/nix-src PR number) to upstream Nix (NixOS/nix). Creates a branch and worktree from origin/master, cherry-picks the commits, builds and tests, drafts a PR description for the user to review, and only after approval pushes the branch and opens the PR. Use when the user asks to upstream, cherry-pick to upstream, or open an upstream PR for a Determinate Nix change.
argument-hint: <commit-rev | nix-src PR number> [more revs/PRs...] [branch-name]
user-invocable: true
---

# Upstreaming a Determinate Nix change to NixOS/nix

Arguments: `$ARGUMENTS`

Each argument is either a commit rev (SHA, tag, branch) in Determinate Nix, or a pull request number of https://github.com/DeterminateSystems/nix-src. Several may be given; they are cherry-picked in the order given. An argument that is neither a rev nor a number is used as the branch name.

Copy this checklist and check off items as you complete them:

```
- [ ] Verify and fetch the remotes
- [ ] Step 1: Determine the commits to cherry-pick
- [ ] Step 2: Create the branch and worktree
- [ ] Step 3: Cherry-pick
- [ ] Step 4: Format and test
- [ ] Step 5: Draft the PR description and report
- [ ] STOP: wait for the user to approve the draft
- [ ] Step 6: Push and create the PR
```

## Remotes

* `origin` is upstream Nix, `git@github.com:NixOS/nix.git`. The new branch is pushed there and the PR is opened against its `master` branch.
* `detsys` is Determinate Nix, `git@github.com:DeterminateSystems/nix-src.git`. Its main branch is `main`.

Verify both with `git remote get-url origin` and `git remote get-url detsys` before doing anything. If `detsys` is missing, add it with `git remote add detsys git@github.com:DeterminateSystems/nix-src.git`. Stop and tell the user if `origin` is not NixOS/nix.

Start by updating both: `git fetch origin master` and `git fetch detsys main`.

## Step 1: Determine the commits to cherry-pick

For a **commit rev**: `git rev-parse --verify '<rev>^{commit}'`. If that fails, run `git fetch detsys <rev>` and retry with `FETCH_HEAD`.

For a **PR number** `N`, query it:

```
gh pr view N --repo DeterminateSystems/nix-src --json number,title,body,url,state,mergeCommit,headRefOid,baseRefName,commits
```

Then pick the commits depending on how the PR was merged (nix-src allows merge commits, squash merges and rebase merges):

* `state` is `MERGED` and `mergeCommit.oid` has two parents (check with `git cat-file -p <oid> | grep -c '^parent '`): it is a real merge commit. The PR's own commits are `git rev-list --reverse --no-merges <oid>^1..<oid>^2`. This excludes commits that were merged from `main` into the PR branch.
* `state` is `MERGED` and the merge commit has one parent and the PR has one commit, or the merge commit's subject matches the PR title with `(#N)` appended: squash merge. Cherry-pick `mergeCommit.oid` itself.
* `state` is `MERGED` and the merge commit has one parent but the PR lists several commits: rebase merge. `mergeCommit.oid` is the last of the rebased commits; take the last `length(commits)` commits ending there, `git rev-list --reverse -n <count> <oid>`, and confirm that their subjects match the subjects of the PR's `commits` list. If they don't match, show both lists to the user and ask.
* `state` is `OPEN`: `git fetch detsys refs/pull/N/head`, then use `git rev-list --reverse --no-merges $(git merge-base detsys/main FETCH_HEAD)..FETCH_HEAD`.
* `state` is `CLOSED` (not merged): tell the user and stop.

Print the resulting list with `git log --oneline --no-walk <commits>` and check it for commits that make no sense upstream: bumps of `.version-determinate`, edits under `doc/manual/source/release-notes-determinate/`, generated Determinate release notes, or merges. Leave those out and say so in the Step 5 report. If a commit is only partly Determinate-specific (for example a code change plus a Determinate release-note file), keep it; the Determinate-only hunks are dealt with during the cherry-pick.

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

## Step 3: Cherry-pick

```
git -C <worktree> cherry-pick <commits...>
```

Do not pass `-x`: Determinate commit hashes mean nothing in the upstream repository; the PR description links to the Determinate PR instead. Keep the original author, message and trailers. In particular keep any `Assisted-by:` trailers, which upstream's contributing guidelines require for AI-assisted commits. Do not add trailers of your own.

### Simple conflicts: resolve them yourself

Resolve and continue (`git add` the files, then `git cherry-pick --continue`, keeping the original commit message) when a conflict is one of these:

* Context drift: neighbouring code changed, includes reordered, a function moved or renamed, whitespace or formatting differences.
* A file that was renamed or moved upstream. Apply the hunk to the file's new location.
* Hunks touching Determinate-only files, such as `doc/manual/source/release-notes-determinate/` or `.version-determinate`. Drop those hunks. If the change is user-visible and deserves a release note upstream, add one under `doc/manual/rl-next/` instead, in the format used by the existing files there.
* Trivial API differences that need a one- or two-line adaptation whose correctness is obvious.

After resolving, rebuild the relevant part if practical, and mention every hand-resolved conflict in the Step 5 report.

### Bigger conflicts: stop and ask

Do not guess when any of these apply:

* The commit relies on functionality that does not exist upstream yet (a class, function, setting, experimental feature, or an earlier Determinate commit that was never upstreamed).
* The same area was redesigned upstream, so the change needs to be reimplemented rather than merged.
* The conflict spans many files or you are unsure that a resolution preserves the intended behaviour.

In that case leave the cherry-pick in its conflicted state (do not abort) and report to the user: which commit, which files, what the missing prerequisite or divergence is, and the options you see, typically skipping that commit, upstreaming the prerequisite first, adapting the change by hand, or abandoning. Ask how to proceed and wait for the answer before doing anything else.

### After the cherry-picks

Review the result:

```
git -C <worktree> log --oneline origin/master..HEAD
git -C <worktree> diff --stat origin/master..HEAD
git -C <worktree> diff origin/master..HEAD | grep -n -i 'determinate\|version-determinate'
```

Anything Determinate-specific that leaked through has to be removed with a fixup to the commit that introduced it. Use `git -C <worktree> commit --fixup <sha>` followed by `GIT_SEQUENCE_EDITOR=true git -C <worktree> rebase -i --autosquash origin/master`.

## Step 4: Format and test

Run the formatter from the dev shell, which is how upstream CI checks formatting:

```
cd <worktree> && nix develop -c ./maintainers/format.sh
```

If it changed files, fold them into the commit that introduced them with the fixup and autosquash procedure above.

Then build and test. Build the default package, which runs the unit tests and functional tests as part of its build:

```
cd <worktree> && nix build -L .
```

This takes a long time. Run it in the background with a generous timeout and wait for it to finish; do not poll. If the change adds or modifies tests, confirm from the build log that they ran. If something fails, decide whether it is caused by the cherry-picked change (fix it, with a fixup into the right commit, and rebuild) or is a pre-existing failure on `origin/master` (check by looking at the failing test on `origin/master`; if it fails there too, say so in the Step 5 report and carry on).

## Step 5: Draft the PR description, then stop for review

Write the PR description to a file in the scratchpad directory, for example `<scratchpad>/upstream-pr-<branch>.md`. Start from upstream's template, `.github/PULL_REQUEST_TEMPLATE.md` in the worktree (not the one in the Determinate checkout), and keep its structure and its `## Motivation` and `## Context` sections.

Content guidelines, matching the user's existing upstream PRs:

* When upstreaming a PR, start from the Determinate PR's own description (the `body` field from `gh pr view`) rather than writing a new one. Reuse its text, adapting it where needed: drop anything that only applies to Determinate Nix (Determinate release notes, Determinate version numbers, internal ticket references such as Sentry reports or Linear issues that upstream readers cannot see, mentions of commits that were left out), and map it onto the upstream template's sections. Keep the author's wording where it still applies.
* For a bare commit rev, find the PR that contains it with `gh pr list --repo DeterminateSystems/nix-src --search <sha> --state all --json number,url,body` and do the same with that PR's description. If there is none, write the Motivation yourself from the commit messages: a few sentences or a short bullet list saying what the change does and why.
* End the Motivation with a link to the Determinate PR, for example `Taken from https://github.com/DeterminateSystems/nix-src/pull/N.` If there is no PR, say the commit was taken from Determinate Nix `main`.
* Context: only fill it in when there is something to say, such as the implementation strategy for a non-trivial change, related upstream issues (search with `gh issue list --repo NixOS/nix --search '...'`), or reading order for a large diff. Leave the template's placeholder comments in place otherwise.
* Leave the rest of the template (the leading comment block and the footer) untouched.
* Title: for a single commit, its subject line; otherwise a concise imperative summary, like a commit subject. Put the proposed title on the first line of the file as `Title: ...` so the user can edit it too, and strip that line before using the file as the body.
* Do not add any line attributing the PR to an agent or tool, such as "Generated with", "Co-Authored-By" or "Assisted-by", to the title or the description. Upstream's contributing guidelines require PR descriptions to be human-authored, which is why the user reviews and edits this draft before it is submitted.

Now report to the user and stop. The report must contain: the branch name and worktree path, the list of cherry-picked commits and any that were dropped, conflicts you resolved by hand, the build and test outcome (with the failing output if something failed), the path of the draft file, and the draft's contents. Ask the user to review or edit the file and to confirm that the PR may be created. Do not push and do not create the PR until they confirm.

## Step 6: After approval, push and create the PR

Re-read the draft file first, since the user may have edited it. Then:

```
git -C <worktree> push -u origin <branch>:refs/heads/<branch>
gh pr create --repo NixOS/nix --base master --head <branch> --title "<title>" --body-file <body-file>
```

Report the PR URL. Leave the worktree in place and mention that it can be removed later with `git worktree remove <worktree>`.
