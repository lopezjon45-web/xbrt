#!/usr/bin/env bash
# run-u0.sh <release dir> [--disc "E:\Xbox\<name>.iso" | --survey "E:\Xbox"] [--bisect] [--deadline N]
#
# <release dir> is a download of one CI prerelease (gh release download build-N -D <dir>) holding the appx files and
# SHA256SUMS.txt. Acts on the console only while ~/mesa-one-s/xbox/console-free reads exactly "xbrt". Never
# uninstalls anything. Runs in the foreground. DRY_RUN=1 does the Mac-side checks and stops.
set -euo pipefail
# shellcheck source=/Users/jonlopez/mesa-one-s/cpugate/lib.sh
. /Users/jonlopez/mesa-one-s/cpugate/lib.sh

HERE=$(cd "$(dirname "$0")" && pwd)
LEASE=/Users/jonlopez/mesa-one-s/xbox/console-free
MAIN_NAME=LopezJon.XboxRuntime
DEADLINE=90
DRY_RUN=${DRY_RUN:-0}
DISC=""
SURVEY=""
BISECT=0

[ $# -ge 1 ] || die "usage: run-u0.sh <release dir> [--disc \"E:\\Xbox\\<name>.iso\" | --survey \"E:\\Xbox\"] [--bisect] [--deadline N]"
ART=$(cd "$1" && pwd)
shift
while [ $# -gt 0 ]; do
    case $1 in
        --disc) DISC=$2; shift ;;
        --survey) SURVEY=$2; shift ;;
        --bisect) BISECT=1 ;;
        --deadline) DEADLINE=$2; shift ;;
        *) die "unknown option $1" ;;
    esac
    shift
done

lease_ok() { [ -e "$LEASE" ] && [ "$(tr -d '[:space:]' < "$LEASE")" = xbrt ]; }

check_sums() {
    local f sum
    [ -f "$ART/SHA256SUMS.txt" ] || die "no SHA256SUMS.txt in $ART"
    for f in "$ART"/*.appx; do
        sum=$(awk -v n="$(basename "$f")" '$2 == n { print $1 }' "$ART/SHA256SUMS.txt")
        [ -n "$sum" ] || die "$(basename "$f") is not in SHA256SUMS.txt"
        [ "$(shasum -a 256 "$f" | awk '{ print $1 }')" = "$sum" ] || die "$f does not match SHA256SUMS.txt"
    done
}

package_named() {
    local f
    f=$(find "$ART" -maxdepth 1 -name "${1}_*_x64.appx" | head -n 1)
    [ -n "$f" ] || die "no ${1}_*_x64.appx in $ART"
    printf '%s' "$f"
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

launch_link() {
    if [ -n "$SURVEY" ]; then
        printf 'xboxrt:?survey=%s&launchOnExit=%s' "$(urlenc "$SURVEY")" "$(urlenc 'decomp:?from=xboxrt')"
    else
        printf 'xboxrt:?disc=%s&launchOnExit=%s' "$(urlenc "$DISC")" "$(urlenc 'decomp:?from=xboxrt')"
    fi
}

run_to_end() {
    local dir="$1" t0=$SECONDS seen=0 absent=0
    start_once "$dir/start-response.txt" || return 1
    while [ $((SECONDS - t0)) -lt "$DEADLINE" ]; do
        sleep 5
        if app_running; then seen=1; absent=0; else absent=$((absent + 1)); fi
        if [ "$seen" = 1 ] && get_file "$P_FULL" '\LocalState' run.txt "$dir/run.txt" && grep -q '^END' "$dir/run.txt"; then
            say "END after $((SECONDS - t0)) s"
            return 0
        fi
        if [ "$absent" -ge 2 ]; then
            say "the process is gone without END after $((SECONDS - t0)) s (seen running: $seen)"
            return 1
        fi
    done
    say "no END within $DEADLINE s"
    return 1
}

check_sums
main_appx=$(package_named "$MAIN_NAME")
version=$(manifest_field "$main_appx" Version)
deps=$(unzip -p "$main_appx" AppxManifest.xml | grep -o '<PackageDependency [^>]*' || true)
[ -z "$deps" ] || die "$main_appx depends on framework packages this script does not install: $deps"
say "package: $(basename "$main_appx") $MAIN_NAME $version, no framework dependencies"
[ -z "$DISC" ] || [ -z "$SURVEY" ] || die "--disc and --survey are one or the other"
[ -z "$DISC$SURVEY" ] || say "link: $(launch_link)"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/Users/jonlopez/mesa-one-s/xbrt-results/$STAMP"
if lease_ok; then say "lease: xbrt"; else say "lease: $LEASE does not read \"xbrt\""; fi
if [ "$DRY_RUN" = 1 ]; then
    say "dry run: would install $MAIN_NAME $version$([ "$BISECT" = 1 ] && printf ' and the six bisect identities'), write launch.txt$([ -z "$DISC$SURVEY" ] && printf ' (none: no --disc or --survey)'), start, wait for END (deadline $DEADLINE s), screenshot, stop, save to $OUT/"
    exit 0
fi

require_tools
lease_ok || die "no lease: $LEASE must read exactly \"xbrt\""
connected
signed_in || die "no user is signed in on the console; ask Jon to sign in first"
mkdir -p "$OUT"
packages_json > "$OUT/packages-before.json"
say "== $MAIN_NAME $version (console about to be used)"

install_identity "$main_appx"
find_installed "$MAIN_NAME"

# a fresh package's LocalState takes portal file calls only after one start and stop
say "warm-up start and stop"
lease_ok || die "the lease was taken back before the warm-up"
start_once "$OUT/warmup-start-response.txt" || say "warm-up start refused (see $OUT/warmup-start-response.txt)"
sleep 8
stop_once
sleep 3
get_file "$P_FULL" '\LocalState' run.txt "$OUT/warmup-run.txt" && say "saved the warm-up's run.txt ($(wc -l < "$OUT/warmup-run.txt" | tr -d ' ') lines)"
del_file "$P_FULL" '\LocalState' run.txt || true
if [ -n "$DISC$SURVEY" ]; then
    printf '%s\n' "$(launch_link)" > "$OUT/launch.txt"
    put_file "$P_FULL" '\LocalState' "$OUT/launch.txt" launch.txt && say "launch.txt written"
fi

lease_ok || die "the lease was taken back before the run"
run_to_end "$OUT" || true
portal -m 60 -o "$OUT/screen.png" "$B/ext/screenshot?download=true" || say "screenshot failed"
stop_once
sleep 2
get_file "$P_FULL" '\LocalState' run.txt "$OUT/run.txt" || say "no run.txt on the console"
get_file "$P_FULL" '\LocalState' shell.txt "$OUT/shell.txt" || true
[ -z "$SURVEY" ] || get_file "$P_FULL" '\LocalState' survey.tsv "$OUT/survey.tsv" || say "no survey.tsv on the console"

if [ "$BISECT" = 1 ]; then
    say "== bisect ladder"
    for i in 0 1 2 3 4 5; do
        name="LopezJon.XbrtBisect$i"
        lease_ok || die "the lease was taken back during the bisect"
        install_identity "$(package_named "$name")"
        find_installed "$name"
        if start_once "$OUT/bisect$i-start-response.txt"; then
            sleep 6
            stop_once
            printf '%s\tstarted\n' "$name" >> "$OUT/bisect.tsv"
        else
            printf '%s\trefused\t%s\n' "$name" "$(tr -d '\n' < "$OUT/bisect$i-start-response.txt" | cut -c1-120)" >> "$OUT/bisect.tsv"
        fi
    done
    cat "$OUT/bisect.tsv"
fi

packages_json > "$OUT/packages.json"
say "run.txt:"
[ -s "$OUT/run.txt" ] && sed 's/^/  /' "$OUT/run.txt" || say "  (none)"
say "results: $OUT"
