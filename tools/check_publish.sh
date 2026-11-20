#!/bin/bash
# Before publishing: looks at exactly the files git WOULD take (tracked +
# untracked, minus .gitignore) and fails if any of them looks like a secret,
# like state, or like the TV manufacturer's code. Works before the first
# `git init` too (it uses a throw-away git directory outside the repository;
# nothing in the repository is changed).
#
# usage: tools/check_publish.sh        exit code 0 = nothing found
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R" || exit 2
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  LIST=$(git ls-files -co --exclude-standard)
else
  G=$(mktemp -d)
  git --git-dir="$G/.git" --work-tree="$R" init -q
  LIST=$(git --git-dir="$G/.git" --work-tree="$R" ls-files -o --exclude-standard)
  rm -rf "$G"
fi
N=$(printf '%s\n' "$LIST" | grep -c .)
echo "checking $N file(s) that would be published"
printf '%s\n' "$LIST" | python3 "$R/tools/check_publish.py" "$R"
