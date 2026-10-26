#!/usr/bin/env bash
# run-m0.sh <release dir> [--launches N] [--deadline S]
#
# The M0 probe on the console (host/m0/probe.h): installs the build's LopezJon.XboxRuntime package, whose payload carries
# m0-corpus.txt, writes launch.txt "m0-probe", and starts the app until LocalState\m0.tsv has its end row. A launch
# that ends with "m0 relaunch" in run.txt, or whose process disappears without END, is started again (m0.tsv tells
# the app where to resume), up to --launches. Then m0.tsv, every launch's run.txt and a screenshot come home, and
# scripts/m0-report.py applies the decision rule.
#
# <release dir> is a download of one CI prerelease (gh release download build-N -D <dir>). Acts on the console only
# while ~/mesa-one-s/xbox/console-free reads exactly "xbrt-m0". Never uninstalls anything. Runs in the foreground.
# DRY_RUN=1 does the Mac-side checks and stops.
set -euo pipefail
# The script runs from a snapshot of itself, so an edit while a run is on cannot break the run (AGENT-RULES.md).
if [ -z "${XBRT_SNAPSHOT:-}" ]; then
    snapshot=$(mktemp -t run-m0.XXXXXX) && cp "$0" "$snapshot" && chmod +x "$snapshot"
    XBRT_SNAPSHOT=1 XBRT_SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) exec bash "$snapshot" "$@"
fi
# shellcheck source=/Users/jonlopez/mesa-one-s/cpugate/lib.sh
. /Users/jonlopez/mesa-one-s/cpugate/lib.sh

HERE=${XBRT_SCRIPT_DIR:-$(cd "$(dirname "$0")" && pwd)}
LEASE=/Users/jonlopez/mesa-one-s/xbox/console-free
LEASE_NAME=xbrt-m0
MAIN_NAME=LopezJon.XboxRuntime
LAUNCHES=25
DEADLINE=900
DRY_RUN=${DRY_RUN:-0}

[ $# -ge 1 ] || die "usage: run-m0.sh <release dir> [--launches N] [--deadline S]"
ART=$(cd "$1" && pwd)
shift
while [ $# -gt 0 ]; do
    case $1 in
        --launches) LAUNCHES=$2; shift ;;
        --deadline) DEADLINE=$2; shift ;;
        *) die "unknown option $1" ;;
    esac
    shift
done

lease_ok() { [ -e "$LEASE" ] && [ "$(tr -d '[:space:]' < "$LEASE")" = "$LEASE_NAME" ]; }

check_sums() {
    local f sum
    [ -f "$ART/SHA256SUMS.txt" ] || die "no SHA256SUMS.txt in $ART"
    for f in "$ART"/*.appx; do
        sum=$(awk -v n="$(basename "$f")" '$2 == n { print $1 }' "$ART/SHA256SUMS.txt")
        [ -n "$sum" ] || die "$(basename "$f") is not in SHA256SUMS.txt"
        [ "$(shasum -a 256 "$f" | awk '{ print $1 }')" = "$sum" ] || die "$f does not match SHA256SUMS.txt"
    done
}

packages_json() { portal -m 20 "$B/api/app/packagemanager/packages"; }

find_installed() {
    local line
    line=$(packages_json | jq -r --arg n "$1" '
        [.InstalledPackages[] | select(.PackageFullName | startswith($n + "_"))]
        | sort_by([.Version.Major, .Version.Minor, .Version.Build, .Version.Revision]) | last // empty
        | [.PackageFullName, .PackageRelativeId, "\(.Version.Major).\(.Version.Minor).\(.Version.Build).\(.Version.Revision)"]
        | @tsv')
    P_FULL=""; P_RELID=""; P_VER=""
    [ -n "$line" ] && IFS=$'\t' read -r P_FULL P_RELID P_VER <<< "$line"
    return 0
}

install_identity() {
    local appx=$1 name version out
    name=$(manifest_field "$appx" Name)
    version=$(manifest_field "$appx" Version)
    find_installed "$name"
    if [ "$P_VER" = "$version" ]; then
        say "already installed: $P_FULL"
        return 0
    fi
    if [ -n "$P_VER" ] && version_lt "$version" "$P_VER"; then
        die "$name $version is below the installed $P_VER ($P_FULL): build a higher version (build.ps1 sets the base)"
    fi
    out=$(install_package "$appx" 2>&1) || true
    say "$out"
    if printf '%s' "$out" | grep -q '80073D02'; then say "install answered 0x80073D02: treated as present"; fi
    find_installed "$name"
    [ "$P_VER" = "$version" ] || die "$name $version is not in the package list after the install"
}

start_once() {
    local tok code
    signed_in || die "no user is signed in on the console; ask Jon to sign in first"
    tok=$(token)
    code=$(portal -m 30 -X POST -H "X-CSRF-Token: $tok" -H "Content-Length: 0" -o "$1" -w '%{http_code}' \
        "$B/api/taskmanager/app?appid=$(b64 "$P_RELID")&package=$(b64 "$P_FULL")")
    say "start HTTP $code $(tr -d '\n' < "$1" | cut -c1-120)"
    [ "$code" = 200 ]
}

stop_once() {
    local code
    code=$(portal -m 30 -X DELETE -H "X-CSRF-Token: $(token)" -o /dev/null -w '%{http_code}' \
        "$B/api/taskmanager/app?package=$(b64 "$P_FULL")")
    case "$code" in 400 | 403)
        code=$(portal -m 30 -X DELETE -H "X-CSRF-Token: $(token)" -o /dev/null -w '%{http_code}' \
            "$B/api/taskmanager/app?package=$(b64 "$P_FULL")&forcestop=yes")
        say "stop needed forcestop: HTTP $code" ;;
    *) say "stop HTTP $code" ;;
    esac
}

# launch_once <n>: start, wait for END in run.txt or for the process to go; prints done, relaunch or gone
launch_once() {
    local n=$1 t0=$SECONDS seen=0 absent=0 run="$OUT/run-$1.txt"
    start_once "$OUT/start-$n.txt" >&2 || { echo gone; return; }
    while [ $((SECONDS - t0)) -lt "$DEADLINE" ]; do
        sleep 10
        lease_ok || die "the lease was taken back during launch $n"
        if app_running; then seen=1; absent=0; else absent=$((absent + 1)); fi
        if [ "$seen" = 1 ] && get_file "$P_FULL" '\LocalState' run.txt "$run" && grep -q '^END' "$run"; then
            say "launch $n: END after $((SECONDS - t0)) s" >&2
            if grep -q '^m0 relaunch' "$run"; then echo relaunch; else echo done; fi
            return
        fi
        if [ "$absent" -ge 2 ]; then
            get_file "$P_FULL" '\LocalState' run.txt "$run" || true
            say "launch $n: the process is gone without END after $((SECONDS - t0)) s (seen running: $seen)" >&2
            echo gone
            return
        fi
    done
    say "launch $n: no END within $DEADLINE s" >&2
    echo timeout
}

check_sums
main_appx=$(find "$ART" -maxdepth 1 -name "${MAIN_NAME}_*_x64.appx" | head -n 1)
[ -n "$main_appx" ] || die "no ${MAIN_NAME}_*_x64.appx in $ART"
version=$(manifest_field "$main_appx" Version)
unzip -l "$main_appx" | grep -q ' m0-corpus.txt$' || die "$main_appx carries no m0-corpus.txt"
say "package: $(basename "$main_appx") $version with m0-corpus.txt ($(unzip -p "$main_appx" m0-corpus.txt | grep -c '^pipeline ') pipelines)"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/Users/jonlopez/mesa-one-s/xbrt-results/m0-$STAMP"
if lease_ok; then say "lease: $LEASE_NAME"; else say "lease: $LEASE does not read \"$LEASE_NAME\""; fi
if [ "$DRY_RUN" = 1 ]; then
    say "dry run: would install $MAIN_NAME $version, warm up, delete m0.tsv, write launch.txt m0-probe, start up to $LAUNCHES times (deadline $DEADLINE s each) until m0.tsv ends, screenshot, stop, save to $OUT/"
    exit 0
fi

require_tools
lease_ok || die "no lease: $LEASE must read exactly \"$LEASE_NAME\""
connected
signed_in || die "no user is signed in on the console; ask Jon to sign in first"
mkdir -p "$OUT"
packages_json > "$OUT/packages-before.json"
say "== $MAIN_NAME $version, M0 probe (console about to be used)"

install_identity "$main_appx"
find_installed "$MAIN_NAME"

say "warm-up start and stop"
lease_ok || die "the lease was taken back before the warm-up"
start_once "$OUT/warmup-start-response.txt" || say "warm-up start refused (see $OUT/warmup-start-response.txt)"
sleep 8
stop_once
sleep 3
for f in run.txt m0.tsv launch.txt; do del_file "$P_FULL" '\LocalState' "$f" || true; done
printf 'm0-probe\n' > "$OUT/launch.txt"
put_file "$P_FULL" '\LocalState' "$OUT/launch.txt" launch.txt && say "launch.txt written (m0-probe)"

result=none
for n in $(seq 1 "$LAUNCHES"); do
    lease_ok || die "the lease was taken back before launch $n"
    result=$(launch_once "$n")
    say "launch $n: $result"
    [ "$result" = done ] && break
    [ "$result" = timeout ] && break
    stop_once || true
    sleep 3
done
portal -m 60 -o "$OUT/screen.png" "$B/ext/screenshot?download=true" || say "screenshot failed"
stop_once
sleep 2
get_file "$P_FULL" '\LocalState' m0.tsv "$OUT/m0.tsv" || say "no m0.tsv on the console"
get_file "$P_FULL" '\LocalState' shell.txt "$OUT/shell.txt" || true
del_file "$P_FULL" '\LocalState' launch.txt || true
packages_json > "$OUT/packages.json"
say "last launch: $result"
[ -s "$OUT/m0.tsv" ] && python3 "$HERE/../scripts/m0-report.py" "$OUT/m0.tsv" --label "One S, $MAIN_NAME $version" |
    tee "$OUT/m0-report.txt" || true
say "results: $OUT"
