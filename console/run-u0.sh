#!/usr/bin/env bash
# run-u0.sh <release dir> [--disc-match <text> | --survey "E:\Xbox" | --upload-xbe <default.xbe>] [--bisect] [--deadline N]
#
# <release dir> is a download of one CI prerelease (gh release download build-N -D <dir>) holding the appx files and
# SHA256SUMS.txt. Acts on the console only while ~/mesa-one-s/xbox/console-free reads exactly "xbrt". Never
# uninstalls anything. Runs in the foreground. DRY_RUN=1 does the Mac-side checks and stops.
set -euo pipefail
# The script runs from a snapshot of itself, so an edit while a run is on cannot break the run (AGENT-RULES.md).
if [ -z "${XBRT_SNAPSHOT:-}" ]; then
    snapshot=$(mktemp -t run-u0.XXXXXX) && cp "$0" "$snapshot" && chmod +x "$snapshot"
    XBRT_SNAPSHOT=1 XBRT_SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) exec bash "$snapshot" "$@"
fi
# shellcheck source=/Users/jonlopez/mesa-one-s/cpugate/lib.sh
. /Users/jonlopez/mesa-one-s/cpugate/lib.sh

HERE=${XBRT_SCRIPT_DIR:-$(cd "$(dirname "$0")" && pwd)}
LEASE=/Users/jonlopez/mesa-one-s/xbox/console-free
MAIN_NAME=LopezJon.XboxRuntime
DEADLINE=90
DRY_RUN=${DRY_RUN:-0}
DISC=""
DISC_MATCH=""
SURVEY=""
UPLOAD_XBE=""
BISECT=0
LIBRARY=/Users/jonlopez/mesa-one-s/xbrt-local/library.txt
DRIVE_MOUNT_FILE=/Users/jonlopez/mesa-one-s/xbrt-local/drive-mount.txt   # the drive's Xbox folder when it is on the Mac; local only

[ $# -ge 1 ] || die "usage: run-u0.sh <release dir> [--disc-match <text> | --survey \"E:\\Xbox\" | --upload-xbe <default.xbe>] [--bisect] [--deadline N]"
ART=$(cd "$1" && pwd)
shift
while [ $# -gt 0 ]; do
    case $1 in
        --disc-match) DISC_MATCH=$2; shift ;;
        --survey) SURVEY=$2; shift ;;
        --upload-xbe) UPLOAD_XBE=$2; shift ;;
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
    # The console refuses a downgrade and this script never uninstalls, so a lower version stops here, by name.
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

# resolve_disc: the one drive listing entry containing DISC_MATCH; the listing comes from the app or the mounted drive
resolve_disc() {
    local listing hits
    if [ -f "$LIBRARY" ]; then
        listing=$(cat "$LIBRARY")
    elif [ -s "$DRIVE_MOUNT_FILE" ] && [ -d "$(cat "$DRIVE_MOUNT_FILE")" ]; then
        listing=$(find "$(cat "$DRIVE_MOUNT_FILE")" -maxdepth 1 \( -iname '*.iso' -o -iname '*.xiso' \) -exec basename {} \; | sed 's|^|E:\\Xbox\\|')
    else
        die "no drive listing: start the app once without a link (it writes LocalState\\library.txt, pulled to $LIBRARY) or mount the drive named in $DRIVE_MOUNT_FILE"
    fi
    hits=$(printf '%s\n' "$listing" | grep -i -F -- "$DISC_MATCH" || true)
    [ "$(printf '%s\n' "$hits" | grep -c .)" = 1 ] || die "--disc-match '$DISC_MATCH' matches $(printf '%s\n' "$hits" | grep -c .) entries, need exactly one:
$hits"
    DISC=$hits
}

# launch_link: the hub's link for the disc, the extracted disc or the survey folder, each value escaped once
launch_link() {
    local back; back=$(urlenc 'decomp:')
    if [ -n "$SURVEY" ]; then
        printf 'xboxrt:?survey=%s&launchOnExit=%s' "$(urlenc "$SURVEY")" "$back"
    elif [ -n "$UPLOAD_XBE" ]; then
        printf 'xboxrt:?dir=disc&launchOnExit=%s' "$back"
    else
        printf 'xboxrt:?disc=%s&home=%s&launchOnExit=%s' "$(urlenc "$DISC")" "$(urlenc 'E:\Game Vault')" "$back"
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
[ "$(printf '%s' "${DISC_MATCH:+1}${SURVEY:+1}${UPLOAD_XBE:+1}" | wc -c | tr -d ' ')" -le 1 ] || die "--disc-match, --survey and --upload-xbe are one or the other"
[ -z "$DISC_MATCH" ] || resolve_disc
[ -z "$UPLOAD_XBE" ] || [ -f "$UPLOAD_XBE" ] || die "$UPLOAD_XBE is not a file"
[ -z "$DISC$SURVEY$UPLOAD_XBE" ] || say "link: $(launch_link)"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/Users/jonlopez/mesa-one-s/xbrt-results/$STAMP"
if lease_ok; then say "lease: xbrt"; else say "lease: $LEASE does not read \"xbrt\""; fi
if [ "$DRY_RUN" = 1 ]; then
    say "dry run: would install $MAIN_NAME $version$([ "$BISECT" = 1 ] && printf ' and the six bisect identities'), write launch.txt$([ -z "$DISC$SURVEY$UPLOAD_XBE" ] && printf ' (none: no --disc-match, --survey or --upload-xbe)'), start, wait for END (deadline $DEADLINE s), screenshot, stop, save to $OUT/"
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
if [ -n "$UPLOAD_XBE" ]; then
    lease_ok || die "the lease was taken back before the upload"
    put_file "$P_FULL" '\LocalState\disc' "$UPLOAD_XBE" default.xbe && say "default.xbe uploaded into LocalState\disc ($(shasum -a 256 "$UPLOAD_XBE" | cut -c1-12))"
fi
if [ -n "$DISC$SURVEY$UPLOAD_XBE" ]; then
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
if get_file "$P_FULL" '\LocalState' library.txt "$OUT/library.txt"; then
    mkdir -p "$(dirname "$LIBRARY")" && cp "$OUT/library.txt" "$LIBRARY" && say "drive listing saved to $LIBRARY ($(grep -c . "$LIBRARY") images)"
fi

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
