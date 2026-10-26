#!/usr/bin/env bash
# check-no-title-code.sh [survey.tsv]: fails when core/, host/ or shell/ branches on a title id (title_id == or !=,
# title_id_hex() ==, a switch on a title id). With a survey.tsv (local only: it names Jon's titles, so never in CI)
# it also fails on any surveyed title id or entry address written as a hex literal there. A title difference must
# reach the runtime as data in TitlePlan, never as code keyed by the title.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
dirs=("$repo/core" "$repo/host" "$repo/shell")
found=0

report() {
    local why=$1 hits=$2
    [ -z "$hits" ] && return 0
    found=1
    echo "$why:"
    echo "$hits" | sed "s|^$repo/|  |"
}

report "title id compared" "$(grep -rnIE \
    -e 'title_id(_hex\(\))?[[:space:]]*(==|!=)' \
    -e '(==|!=)[[:space:]]*[A-Za-z_][A-Za-z0-9_.>-]*title_id' \
    -e 'switch[[:space:]]*\(.*title_id' \
    "${dirs[@]}" || true)"

if [ $# -ge 1 ]; then
    survey=$1
    header=$(head -n 1 "$survey")
    case "$header" in
    image$'\t'title$'\t'title_id$'\t'entry$'\t'*) ;;
    *) echo "$survey: not a survey.tsv (the header does not start image, title, title_id, entry)"; exit 1 ;;
    esac
    values=$(tail -n +2 "$survey" | cut -f 3,4 | tr '\t' '\n' | sed -e 's/^0[xX]//' -e 's/^0*//' | grep -E '^[0-9A-Fa-f]+$' | sort -u || true)
    for v in $values; do
        report "0x$v from $survey" "$(grep -rnIiE "0x0*$v([^0-9a-f]|$)" "${dirs[@]}" || true)"
    done
fi

if [ $found -ne 0 ]; then
    echo "check-no-title-code: per-title code found; make the difference data in TitlePlan instead"
    exit 1
fi
echo "check-no-title-code: no per-title code in core/, host/, shell/${1:+ (and no title id or entry from $1)}"
