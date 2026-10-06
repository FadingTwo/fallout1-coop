#!/usr/bin/env bash
# Headless regression runs with the autotest harness.
#
#   tools/regress.sh run <fallout-ce binary> <out dir> [config overrides...]
#   tools/regress.sh compare <out dir A> <out dir B>
#   tools/regress.sh load <fallout-ce binary> <regress out dir> <out dir> [overrides...]
#
# `run` needs FALLOUT_DATA pointing at a Fallout install (MASTER.DAT,
# CRITTER.DAT, DATA/). The install is never written to: .DAT files are
# symlinked and DATA/ is copied (without saves) into <out dir>.
#
# Config overrides use the game's command line syntax, e.g.
#   "[coop]enabled=1" "[coop]selftest=1"
#
# `compare` checks that two runs produced identical player state dumps and
# identical save games (SAVE.DAT after its header, which holds a timestamp,
# plus every other file in the save slots).
#
# `load` loads the save made by an earlier `run` (possibly another build)
# and checks the loaded state equals that run's state dump.

set -euo pipefail

# SAVE.DAT header: signature, version, names, timestamps and thumbnail.
SAVE_HEADER_SIZE=30051

die() {
    echo "regress: $*" >&2
    exit 2
}

# GNU timeout where the system's timeout is uutils': that one crashed now and
# then in long runs (SIGSEGV), taking the game with it.
timeout_bin=timeout
command -v gnutimeout >/dev/null && timeout_bin=gnutimeout

find_file() {
    find "$1" -maxdepth 1 -iname "$2" -print -quit
}

cmd_run() {
    local script=${AUTOTEST_SCRIPT:-regress}
    [ $# -ge 2 ] || die "usage: run <binary> <out dir> [overrides...]"
    [ -n "${FALLOUT_DATA:-}" ] || die "FALLOUT_DATA is not set"

    local bin out
    bin=$(realpath "$1")
    out=$2
    shift 2

    local master critter data
    master=$(find_file "$FALLOUT_DATA" master.dat)
    critter=$(find_file "$FALLOUT_DATA" critter.dat)
    data=$(find_file "$FALLOUT_DATA" data)
    [ -n "$master" ] && [ -n "$critter" ] && [ -n "$data" ] || die "no game data in $FALLOUT_DATA"

    rm -rf "$out"
    mkdir -p "$out"
    ln -s "$master" "$out/master.dat"
    ln -s "$critter" "$out/critter.dat"
    cp -r "$data" "$out/data"
    rm -rf "$out/data/SAVEGAME" "$out/data/savegame"
    if [ -n "${AUTOTEST_SAVES:-}" ]; then
        cp -r "$AUTOTEST_SAVES/data/SAVEGAME" "$out/data/SAVEGAME"
    fi
    # A crash report left by an earlier run (AUTOTEST_CRASH_REPORT=file).
    if [ -n "${AUTOTEST_CRASH_REPORT:-}" ]; then
        cp "$AUTOTEST_CRASH_REPORT" "$out/crash-report.txt"
    fi
    # Screen size (f1_res.ini), e.g. AUTOTEST_RES=1024x768.
    if [ -n "${AUTOTEST_RES:-}" ]; then
        printf '[MAIN]\nSCR_WIDTH=%s\nSCR_HEIGHT=%s\nWINDOWED=1\n' "${AUTOTEST_RES%x*}" "${AUTOTEST_RES#*x}" >"$out/f1_res.ini"
    fi
    # Simulates different game data (e.g. another patch level).
    if [ -n "${AUTOTEST_TAMPER:-}" ]; then
        echo tampered >"$out/data/coop_tamper.txt"
    fi

    local rc=0
    (
        cd "$out"
        SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy DEBUGACTIVE=log \
            "$timeout_bin" "${AUTOTEST_TIMEOUT:-300}" "$bin" "[autotest]script=$script" "$@" >stdout.log 2>&1
    ) || rc=$?

    # Keep only the saves; the copied game data is large.
    find "$out/data" -mindepth 1 -maxdepth 1 ! -iname savegame -exec rm -rf {} +
    cat "$out/autotest/result.txt" 2>/dev/null || echo "regress: no result.txt (exit $rc)"
    return $rc
}

cmd_compare() {
    [ $# -eq 2 ] || die "usage: compare <out dir A> <out dir B>"
    local a=$1 b=$2 failed=0

    for f in state1.txt state2.txt; do
        if ! cmp -s "$a/autotest/$f" "$b/autotest/$f"; then
            echo "DIFF autotest/$f"
            diff "$a/autotest/$f" "$b/autotest/$f" | head -20 || true
            failed=1
        fi
    done

    local slots
    slots=$(cd "$a/data" && find . -ipath '*savegame*' -type f | sort)
    [ -n "$slots" ] || { echo "no save files in $a"; return 1; }

    if [ "$slots" != "$(cd "$b/data" && find . -ipath '*savegame*' -type f | sort)" ]; then
        echo "DIFF save file lists"
        failed=1
    fi

    local f
    for f in $slots; do
        if [ "$(basename "$f" | tr a-z A-Z)" = SAVE.DAT ]; then
            if ! cmp -s <(tail -c +$((SAVE_HEADER_SIZE + 1)) "$a/data/$f") <(tail -c +$((SAVE_HEADER_SIZE + 1)) "$b/data/$f"); then
                echo "DIFF $f (after header)"
                failed=1
            fi
        elif ! cmp -s "$a/data/$f" "$b/data/$f"; then
            echo "DIFF $f"
            failed=1
        fi
    done

    if [ $failed -eq 0 ]; then
        echo "IDENTICAL: state dumps and $(echo "$slots" | wc -l) save files"
    fi
    return $failed
}

cmd_load() {
    [ $# -ge 3 ] || die "usage: load <binary> <regress out dir> <out dir> [overrides...]"
    local bin=$1 from=$2 out=$3
    shift 3

    local rc=0
    AUTOTEST_SCRIPT=load AUTOTEST_SAVES=$from cmd_run "$bin" "$out" "$@" || rc=$?
    [ $rc -eq 0 ] || return $rc

    if cmp -s "$from/autotest/state1.txt" "$out/autotest/state_loaded.txt"; then
        echo "IDENTICAL: loaded state matches $from"
    else
        echo "DIFF loaded state vs $from/autotest/state1.txt"
        diff "$from/autotest/state1.txt" "$out/autotest/state_loaded.txt" | head -20 || true
        return 1
    fi
}

case "${1:-}" in
run)
    shift
    cmd_run "$@"
    ;;
compare)
    shift
    cmd_compare "$@"
    ;;
load)
    shift
    cmd_load "$@"
    ;;
*)
    die "usage: $0 run|compare|load ..."
    ;;
esac
