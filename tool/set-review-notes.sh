#!/usr/bin/env bash
# Push ios/REVIEW-NOTES.md into the App Store review notes field.
#
#   tool/set-review-notes.sh <version-id>
#
# Only the part AFTER the first horizontal rule is sent. The top of that file
# is a note to ourselves about when to resend it, and a reviewer opening the
# notes to find "Paste the whole of this into Review Notes" learns nothing
# except that we were careless.
set -euo pipefail
VER="${1:?usage: set-review-notes.sh <version-id>}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

NOTES=$(awk 'f{print} /^---$/{f=1}' "$HERE/ios/REVIEW-NOTES.md" | sed '/./,$!d')
n=${#NOTES}
[ "$n" -le 4000 ] || { echo "review notes are $n characters; Apple's limit is 4000" >&2; exit 1; }

# details-update is addressed by the DETAIL id, not the version id, and the
# detail is created lazily -- so resolve it, and create it if this version has
# never had one.
DETAIL=$(asc review details-for-version --version-id "$VER" 2>/dev/null \
         | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["id"])' 2>/dev/null || true)
if [ -z "$DETAIL" ]; then
    echo "no review detail for $VER yet; create it with asc review details-create" >&2
    exit 1
fi
asc review details-update --id "$DETAIL" --notes "$NOTES" >/dev/null
echo "review notes updated on detail $DETAIL: $n characters"
