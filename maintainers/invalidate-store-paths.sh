#!/usr/bin/env bash
set -euo pipefail

# Nix base32 alphabet (no e, o, t, u)
HASH_CHARS='0123456789abcdfghijklmnpqrsvwxyz'

git ls-files -z |
while IFS= read -r -d '' file; do
  # Skip symlinks
  if [ -L "$file" ]; then
    continue
  fi

  perl -pi -e '
    s{
      (['"$HASH_CHARS"']{31})  # prefix + first 31 hash chars
      ['"$HASH_CHARS"']                   # last hash char
      -                                   # hyphen
    }{$1o-}gx
  ' "$file"
done
