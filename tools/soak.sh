#!/usr/bin/env bash
# Round-the-clock (or nightly) testing. Each run draws at random:
#   - what: the tour of every map, the session test (join, leave, rejoin,
#     load, trade, a fight with the turn timer) or a fight with chat;
#   - how: direct, through a local relay, through a slow relay (a line of
#     150-600 KB/s), the address sanitizer, or a Windows player 2 (Wine);
#   - screen sizes for both players, and a seed (map order, where player 2
#     walks).
# One line per run in <out>/summary.log, with everything needed to repeat
# it; failed runs keep their logs in <out>/fail-<n>, passed ones are removed.
#
#   nice tools/soak.sh <out dir>
#
# SOAK_UNTIL=HH:MM stops starting runs at that time (the next time it comes),
# SOAK_RUNS=n after n runs. Needs FALLOUT_DATA (see regress.sh). Optional:
# SOAK_ASAN_BIN, SOAK_WINE (a Wine runner script for the Windows build),
# DISPLAY for Wine.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out"
bin="$here/../build/fallout-ce"

until_epoch=0
if [ -n "${SOAK_UNTIL:-}" ]; then
    until_epoch=$(date -d "${SOAK_UNTIL}" +%s)
    [ "$until_epoch" -le "$(date +%s)" ] && until_epoch=$(date -d "tomorrow ${SOAK_UNTIL}" +%s)
fi

relay_port=27980
slow_port=27981
python3 "$here/coop_server.py" --port 18090 --bind 127.0.0.1 --root "$out/server" \
    --relay-port $relay_port --relay-bind 127.0.0.1 >"$out/server.log" 2>&1 &
server=$!
slow_server=
trap 'kill $server $slow_server 2>/dev/null' EXIT

start_slow_server() {
    kill $slow_server 2>/dev/null
    COOP_RELAY_TEST_RATE=$1 python3 "$here/coop_server.py" --port 18091 --bind 127.0.0.1 --root "$out/server-slow" \
        --relay-port $slow_port --relay-bind 127.0.0.1 >"$out/server-slow.log" 2>&1 &
    slow_server=$!
    sleep 1
}
sleep 1

sizes=(640x480 800x600 1024x768 1280x720 1366x768 1920x1080)
pick() { local list=("$@"); echo "${list[$((RANDOM % ${#list[@]}))]}"; }

n=$(grep -c " run " "$out/summary.log" 2>/dev/null || echo 0)
runs=0
while true; do
    [ -n "${SOAK_RUNS:-}" ] && [ $runs -ge "$SOAK_RUNS" ] && break
    [ "$until_epoch" -gt 0 ] && [ "$(date +%s)" -ge "$until_epoch" ] && break
    n=$((n + 1))
    runs=$((runs + 1))

    seed=$((RANDOM * 32768 + RANDOM))
    RANDOM=$seed
    what=$(pick tour tour session session combat)
    how=$(pick direct relay relay slow asan wine)
    hostRes=$(pick "${sizes[@]}")
    clientRes=$(pick "${sizes[@]}")
    rate=0

    hostBin=$bin
    clientBin=$bin
    extra=(COOP_AUTOTEST_SEED=$seed ASAN_OPTIONS=detect_leaks=0)
    # Every third run checks the engine heap after each heap call.
    heapCheck=$((RANDOM % 3 == 0))
    [ $heapCheck = 1 ] && extra+=(COOP_HEAP_CHECK=1)
    case $how in
    asan)
        if [ -z "${SOAK_ASAN_BIN:-}" ]; then how=direct; else hostBin=$SOAK_ASAN_BIN; clientBin=$SOAK_ASAN_BIN; fi
        ;;
    wine)
        if [ -z "${SOAK_WINE:-}" ]; then how=direct; else clientBin=$SOAK_WINE; fi
        ;;
    relay)
        extra+=(COOP_AUTOTEST_RELAY=http://127.0.0.1:$relay_port/fallout-coop/relay CLIENT_EXTRA="[coop]host=192.0.2.1")
        ;;
    slow)
        rate=$(((RANDOM % 10 + 3) * 50000))
        start_slow_server $rate
        extra+=(COOP_AUTOTEST_RELAY=http://127.0.0.1:$slow_port/fallout-coop/relay CLIENT_EXTRA="[coop]host=192.0.2.1")
        ;;
    esac
    extra+=(COOP_AUTOTEST_RELAY_FILE="$out/code-$n.txt")

    case $what in
    tour) scripts=(net_tour_host net_tour_client); extra+=(COOP_AUTOTEST_TOUR=ALL COOP_AUTOTEST_ROUGH=1 HOST_EXTRA="[coop]turn_time=10") ;;
    session) scripts=(net_session_host net_session_client); extra+=(HOST_EXTRA="[coop]turn_time=3") ;;
    combat) scripts=(net_combat_host net_combat_client); extra+=(COOP_AUTOTEST_COMBAT_CHAT=1) ;;
    esac

    dir="$out/run-$n"
    rm -rf "$dir"
    start=$(date +%s)
    env "${extra[@]}" CLIENT_BIN="$clientBin" HOST_RES="$hostRes" CLIENT_RES="$clientRes" \
        AUTOTEST_TIMEOUT=10800 HOST_WAIT=15 \
        "$here/nettest.sh" "$hostBin" "$dir" "${scripts[@]}" >"$dir.txt" 2>&1
    seconds=$(($(date +%s) - start))

    host=$(grep -h "result:" "$dir/host/autotest/result.txt" 2>/dev/null | tail -1)
    client=$(grep -h "result:" "$dir/client/autotest/result.txt" 2>/dev/null | tail -1)
    maps=$(grep -h "visited" "$dir/host/autotest/result.txt" 2>/dev/null | tail -1)
    asan=$(grep -l "ERROR: AddressSanitizer" "$dir"/*/stdout.log 2>/dev/null | wc -l)
    status=PASS
    if [[ "$host" != *"PASS"* || "$client" != *"PASS"* || "$asan" != "0" ]]; then
        status=FAIL
    fi

    line="run $n $what $how"
    [ "$how" = slow ] && line="$line ${rate}B/s"
    [ $heapCheck = 1 ] && line="$line heapcheck"
    echo "$(date '+%F %T') $line $hostRes/$clientRes seed $seed: $status (${maps:-$what}, ${seconds}s, asan errors $asan)" >>"$out/summary.log"
    if [ $status = FAIL ]; then
        grep -h "FAIL" "$dir"/*/autotest/result.txt 2>/dev/null | head -5 | sed 's/^/    /' >>"$out/summary.log"
        grep -h "HEAP CHECK" "$dir"/*/stdout.log 2>/dev/null | head -3 | sed 's/^/    /' >>"$out/summary.log"
        mv "$dir" "$out/fail-$n"
        mv "$dir.txt" "$out/fail-$n.txt" 2>/dev/null
        # Keep the newest SOAK_KEEP_FAILS (30) failures' logs.
        ls -d "$out"/fail-[0-9]* 2>/dev/null | grep -v '\.txt$' | sort -V | head -n -"${SOAK_KEEP_FAILS:-30}" | while read -r old; do
            rm -rf "$old" "$old.txt" "$old.host.txt"
        done
    else
        rm -rf "$dir" "$dir.txt" "$dir.host.txt"
    fi
done
