#!/usr/bin/env bash
# Copies coop_server.py (and coop_forum.py, if present) to the co-op server
# and restarts it, but only while nobody plays through the relay (a restart
# drops those games). FORCE=1 restarts anyway.
#
#   DEPLOY_TARGET=user@host [DEPLOY_KEY=~/.ssh/key] tools/deploy-server.sh
#
# The files go to ~/fallout-coop/ on the target, and the server runs as
# the systemd unit fallout-coop-report there (see COOP.md, "Running your
# own server").
set -euo pipefail

target=${DEPLOY_TARGET:?set DEPLOY_TARGET=user@host}
here=$(dirname "$0")
key_args=()
[ -n "${DEPLOY_KEY:-}" ] && key_args=(-i "$DEPLOY_KEY")
ssh_cmd=(ssh "${key_args[@]}" "$target")

status=$("${ssh_cmd[@]}" 'curl -s http://127.0.0.1:5178/fallout-coop/status')
playing=$(python3 -c 'import json,sys; s=json.loads(sys.argv[1]); print(len(s["now"]["relayed_games"]))' "$status")
if [ "$playing" != "0" ] && [ -z "${FORCE:-}" ]; then
    echo "deploy-server: $playing game(s) in progress through the relay; not restarting (FORCE=1 to do it anyway)"
    exit 1
fi

files=("$here/coop_server.py")
[ -f "$here/coop_forum.py" ] && files+=("$here/coop_forum.py")
scp -q "${key_args[@]}" "${files[@]}" "$target:fallout-coop/"
"${ssh_cmd[@]}" 'sudo systemctl restart fallout-coop-report; sleep 1; systemctl is-active fallout-coop-report'
