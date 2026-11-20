#!/usr/bin/env bash

# shellcheck source=common.sh
source common.sh

requireGit

# Create a test repo with multiple commits for all our tests
createGitRepo "$TEST_ROOT/shallow-parent"

# Add several commits to have history
echo "{ outputs = _: {}; }" > "$TEST_ROOT/shallow-parent/flake.nix"
echo "" > "$TEST_ROOT/shallow-parent/file.txt"
git -C "$TEST_ROOT/shallow-parent" add file.txt flake.nix
git -C "$TEST_ROOT/shallow-parent" commit -m "First commit"

echo "second" > "$TEST_ROOT/shallow-parent/file.txt"
git -C "$TEST_ROOT/shallow-parent" commit -m "Second commit" -a

echo "third" > "$TEST_ROOT/shallow-parent/file.txt"
git -C "$TEST_ROOT/shallow-parent" commit -m "Third commit" -a

# Add a branch for testing ref fetching
git -C "$TEST_ROOT/shallow-parent" checkout -b dev
echo "branch content" > "$TEST_ROOT/shallow-parent/branch-file.txt"
git -C "$TEST_ROOT/shallow-parent" add branch-file.txt
git -C "$TEST_ROOT/shallow-parent" commit -m "Branch commit"

# Make a shallow clone (depth=1)
git clone --depth 1 "file://$TEST_ROOT/shallow-parent" "$TEST_ROOT/shallow-clone"

# Test 1: Fetching a shallow repo succeeds for outPath because revCount is lazy.
path1=$(nix eval --impure --raw --expr "(builtins.fetchGit { url = \"$TEST_ROOT/shallow-clone\"; ref = \"dev\"; shallow = true; }).outPath")
[[ -d "$path1" ]]
# But accessing revCount on a shallow clone fails.
(! nix eval --impure --expr "(builtins.fetchGit { url = \"$TEST_ROOT/shallow-clone\"; ref = \"dev\"; }).revCount" 2>/dev/null)

# Test 2: But you can request a shallow clone, which won't return a revCount.
path=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"git\"; url = \"file://$TEST_ROOT/shallow-clone\"; ref = \"dev\"; shallow = true; }).outPath")
# Verify file from dev branch exists
[[ -f "$path/branch-file.txt" ]]
# Verify revCount is missing
[[ $(nix eval --impure --expr "(builtins.fetchTree { type = \"git\"; url = \"file://$TEST_ROOT/shallow-clone\"; ref = \"dev\"; shallow = true; }).revCount or 123") == 123 ]]

# Test 3: Check unlocked input error message
expectStderr 1 nix eval --expr 'builtins.fetchTree { type = "git"; url = "file:///foo"; }' | grepQuiet "'fetchTree' doesn't fetch unlocked input"

# Test 4: Regression test for revCount in worktrees derived from shallow clones
# Add a worktree to the shallow clone
git -C "$TEST_ROOT/shallow-clone" worktree add "$TEST_ROOT/shallow-worktree"

# Prior to the fix, this would error out because of the shallow clone's
# inability to find parent commits. Now it should return an error.
if nix eval --impure --expr "(builtins.fetchGit { url = \"file://$TEST_ROOT/shallow-worktree\"; }).revCount" 2>/dev/null; then
    echo "fetchGit unexpectedly succeeded on shallow clone" >&2
    exit 1
fi

# Also verify that fetchTree fails similarly
if nix eval --impure --expr "(builtins.fetchTree { type = \"git\"; url = \"file://$TEST_ROOT/shallow-worktree\"; }).revCount" 2>/dev/null; then
    echo "fetchTree unexpectedly succeeded on shallow clone" >&2
    exit 1
fi

# Verify that we can shallow fetch the worktree
git -C "$TEST_ROOT/shallow-worktree" rev-list --count HEAD >/dev/null
nix eval --impure --raw --expr "(builtins.fetchGit { url = \"file://$TEST_ROOT/shallow-worktree\"; shallow = true; }).rev"

# Test 5: nix build --dry-run on a shallow clone must not force revCount.
# The flake fingerprint must not eagerly evaluate revCount, because that
# would fail (or produce wrong results) on shallow clones.
# Nor should it generate a complete lock file that serializes the root node.
# We remove the parent repo to ensure that a future improvement that
# tries to fetch missing history can't paper over the issue.
createGitRepo "$TEST_ROOT/shallow-build-parent"
echo "" > "$TEST_ROOT/shallow-build-parent/file.txt"
git -C "$TEST_ROOT/shallow-build-parent" add file.txt
git -C "$TEST_ROOT/shallow-build-parent" commit -m "first"
cat > "$TEST_ROOT/shallow-build-parent/flake.nix" <<EOF
{
  outputs = { self }: {
    packages.$system.default =
      derivation {
        name = "test";
        system = "$system";
        builder = "/bin/sh";
        args = [ "-c" "echo ok > \\\$out" ];
      };
  };
}
EOF
git -C "$TEST_ROOT/shallow-build-parent" add flake.nix
git -C "$TEST_ROOT/shallow-build-parent" commit -m "add flake"
git clone --depth 1 "file://$TEST_ROOT/shallow-build-parent" "$TEST_ROOT/shallow-build-clone"
rm -rf "$TEST_ROOT/shallow-build-parent"
nix build --dry-run "git+file://$TEST_ROOT/shallow-build-clone?shallow=1"

# Test 6: Fetching a clean, shallow working tree without `shallow = true`
# fails right away, rather than when `revCount` is forced.
expectStderr 1 nix build --dry-run "git+file://$TEST_ROOT/shallow-build-clone" \
    | grepQuiet "shallow repositories are only allowed when"

# Test 7: A flake with `inputs.self.submodules = true` and a shallow
# submodule checkout works, whether the tree is clean or dirty.
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=protocol.file.allow
export GIT_CONFIG_VALUE_0=always

createGitRepo "$TEST_ROOT/shallow-sub"
for i in 1 2 3; do
    echo "$i" > "$TEST_ROOT/shallow-sub/file"
    git -C "$TEST_ROOT/shallow-sub" add file
    git -C "$TEST_ROOT/shallow-sub" commit -m "$i"
done

createGitRepo "$TEST_ROOT/shallow-sub-parent"
cat > "$TEST_ROOT/shallow-sub-parent/flake.nix" <<EOF
{
  inputs.self.submodules = true;
  outputs = { self }: { x = builtins.readFile (self + "/sub/file"); };
}
EOF
git -C "$TEST_ROOT/shallow-sub-parent" submodule add --depth 1 "file://$TEST_ROOT/shallow-sub" sub
git -C "$TEST_ROOT/shallow-sub-parent" add flake.nix
git -C "$TEST_ROOT/shallow-sub-parent" commit -m "first"
[[ $(git -C "$TEST_ROOT/shallow-sub-parent/sub" rev-parse --is-shallow-repository) = true ]]

[[ $(nix eval --no-write-lock-file --raw "$TEST_ROOT/shallow-sub-parent#x") = 3 ]]

echo "# dirty" >> "$TEST_ROOT/shallow-sub-parent/flake.nix"
[[ $(nix eval --no-write-lock-file --raw "$TEST_ROOT/shallow-sub-parent#x") = 3 ]]
