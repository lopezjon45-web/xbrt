#!/usr/bin/env bash
# check-no-disc-data.sh [--history <git range>]: nothing from Jon's discs or his console may enter the repository.
#
# The tree check lists tracked files and the untracked files `git add -A` would add, and fails on any path matching
# scripts/disc-data-patterns.txt (images, XBEs, packages, keys, console output). The content check greps every such
# file for the generic strings below and, when the local-only list exists, for the title ids, game names and console
# strings it holds; that list lives outside the repository on purpose, so the banned strings never appear here.
# With --history the same content check runs over `git log -p` of the range (the pre-push hook passes the commits
# about to leave the machine). CI runs the tree and content checks; the hook runs all three.
set -euo pipefail
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_PREFIX
repo=$(cd "$(dirname "$0")/.." && pwd)
patterns="$repo/scripts/disc-data-patterns.txt"
local_list=${XBRT_BANNED_PATTERNS:-/Users/jonlopez/mesa-one-s/xbrt-local/banned-patterns.txt}
status=0

generic_regex='\.xiso\.iso|192\.168\.[0-9]+\.[0-9]+:11443|LocalState\\\\launch\.txt:|PackageFullName"'
content_regex=$generic_regex
if [ -f "$local_list" ]; then
    local_regex=$(grep -v -E '^\s*(#|$)' "$local_list" | sed 's/[.[\*^$]/\\&/g' | paste -sd '|' -)
    [ -z "$local_regex" ] || content_regex="$generic_regex|$local_regex"
fi

if [ "${1:-}" = "--history" ]; then
    range=${2:?usage: check-no-disc-data.sh --history <git range>}
    hits=$(git -C "$repo" log -p --no-color "$range" -- . ':!external' ':!scripts/check-no-disc-data.sh' | grep -n -E "$content_regex" | grep -v '^[0-9]*:[-+]\{3\}' | head -20 || true)
    if [ -n "$hits" ]; then
        echo "FAIL disc or console data in the commits $range:"
        echo "$hits" | cut -c1-160
        exit 1
    fi
    echo "no disc data in the history $range"
    exit 0
fi

specs=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    specs+=(":(icase)$line")
done < "$patterns"
[ ${#specs[@]} -gt 0 ] || { echo "FAIL no patterns in $patterns"; exit 1; }

while IFS= read -r path; do
    [ -n "$path" ] || continue
    echo "FAIL tracked: $path"
    status=1
done < <(git -C "$repo" ls-files -- "${specs[@]}")
while IFS= read -r -d '' path; do
    echo "FAIL git add -A would add: $path"
    status=1
done < <(git -C "$repo" ls-files -o --exclude-standard -z -- "${specs[@]}")

while IFS= read -r -d '' path; do
    case "$path" in external/*|core/gen/*|scripts/check-no-disc-data.sh) continue ;; esac
    if hit=$(grep -n -E "$content_regex" "$repo/$path" | head -3); then
        echo "FAIL disc or console data in $path:"
        echo "$hit" | cut -c1-160
        status=1
    fi
done < <(git -C "$repo" ls-files -z; git -C "$repo" ls-files -o --exclude-standard -z)

[ $status -eq 0 ] && echo "no disc data: pass$([ -f "$local_list" ] || echo ' (generic patterns only: no local list)')"
exit $status
