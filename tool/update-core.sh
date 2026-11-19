#!/usr/bin/env bash
# Merge a newer upstream DOSBox-X into core/.
#
#   tool/update-core.sh            merge the latest upstream release tag
#   tool/update-core.sh <ref>      merge a specific tag, branch or commit
#   tool/update-core.sh --finish   after resolving conflicts in the work tree
#
# core/ is our fork (the SDL3 port and retrodos_host) kept as plain files.
# core/UPSTREAM records the upstream commit it was last merged with. This does
# a real three-way merge: that commit is the base, core/ as it is now is ours,
# and the new upstream is theirs. Only conflicting hunks need attention.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(dirname "$HERE")"
CORE="$APP/core"
WORK="${WORK:-$HOME/.cache/retro-dosbox-core-update}"

field() { sed -n "s/^$1=//p" "$CORE/UPSTREAM"; }
REPO="$(field repo)"

finish() {
    cd "$WORK"
    if [ -n "$(git diff --name-only --diff-filter=U)" ]; then
        echo "error: unresolved conflicts remain in $WORK:" >&2
        git diff --name-only --diff-filter=U >&2; exit 1
    fi
    git rev-parse -q --verify MERGE_HEAD >/dev/null &&
        git -c user.name=update-core -c user.email=update-core@localhost commit -q --no-edit
    local ref commit
    ref="$(cat .update-ref)"; commit="$(git rev-parse "$ref^{commit}")"
    find "$CORE" -mindepth 1 -maxdepth 1 ! -name UPSTREAM -exec rm -rf {} +
    git archive HEAD | tar -x -C "$CORE"
    sed -i -e "s|^ref=.*|ref=$ref|" -e "s|^commit=.*|commit=$commit|" "$CORE/UPSTREAM"
    echo "==> core/ now merged up to $ref ($commit)"
    echo "    rebuild (tool/build-linux.sh), then commit core/ in this repository."
}

if [ "${1:-}" = "--finish" ]; then finish; exit 0; fi

BASE="$(field commit)"
rm -rf "$WORK"
git clone -q --no-checkout "$REPO" "$WORK"
cd "$WORK"
TARGET="${1:-$(git tag -l 'dosbox-x-v*' --sort=-v:refname | grep -v -i -E 'rc|beta' | head -1)}"
echo "==> base   $BASE ($(field ref))"
echo "==> target $TARGET"
if git merge-base --is-ancestor "$TARGET^{commit}" "$BASE"; then
    echo "core/ already contains $TARGET"; exit 0
fi
echo "$TARGET" > .update-ref
git checkout -q -b ours "$BASE"
git rm -rq --cached . && find . -mindepth 1 -maxdepth 1 ! -name .git ! -name .update-ref -exec rm -rf {} +
(cd "$CORE" && tar -c --exclude=./UPSTREAM .) | tar -x
git add -A . ':!.update-ref'
git -c user.name=update-core -c user.email=update-core@localhost commit -q -m "Retro-DOS core as of $(field ref)"
if git -c user.name=update-core -c user.email=update-core@localhost merge -q --no-edit "$TARGET"; then
    finish
else
    echo >&2
    echo "Conflicts. Resolve them in $WORK, git add the files, then run:" >&2
    echo "    tool/update-core.sh --finish" >&2
    exit 1
fi
