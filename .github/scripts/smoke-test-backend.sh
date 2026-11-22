#!/usr/bin/env bash
# Start the desktop backend the way the shell does and check that it answers.
#
#   .github/scripts/smoke-test-backend.sh [PYTHON]
#
# PYTHON defaults to api/.venv/bin/python. The data folder is a temporary directory, so the test
# never touches a real library.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${1:-$ROOT/api/.venv/bin/python}"
PORT="${VBLIS_SMOKE_PORT:-8123}"
DATA="$(mktemp -d -t vblis-smoke-data-XXXXXX)"
LOG="$(mktemp -t vblis-smoke-backend-XXXXXX.log)"
FS_JSON="$(mktemp -t vblis-smoke-fs-XXXXXX.json)"

[ -x "$PY" ] || { echo "no Python at $PY" >&2; exit 1; }

cleanup() {
    if [ -n "${PID:-}" ]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$DATA" "$FS_JSON"
}
trap cleanup EXIT

echo "Starting backend on 127.0.0.1:$PORT (log $LOG)…"
(
    cd "$ROOT/api"
    VBLIS_DATA_DIR="$DATA" VBLIS_API_PORT="$PORT" VBLIS_WEB_DIR="$ROOT/vsite/dist" \
        exec "$PY" main.py
) >"$LOG" 2>&1 &
PID=$!

ready=0
for _ in $(seq 1 90); do
    if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/models" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        break
    fi
    sleep 1
done

if [ "$ready" -ne 1 ]; then
    echo "Backend did not answer on /models; last 30 log lines:" >&2
    tail -30 "$LOG" >&2
    exit 1
fi

# The desktop folder picker reads this route; it must be mounted in the desktop app (not only in the
# network app) and must list directories.
curl -fsS "http://127.0.0.1:$PORT/fs/list" -o "$FS_JSON"
"$PY" - "$FS_JSON" <<'PY'
import json
import sys
with open(sys.argv[1]) as fh:
    listing = json.load(fh)
assert isinstance(listing.get("path"), str) and listing["path"].startswith("/"), listing
assert isinstance(listing.get("entries"), list), listing
assert all("name" in entry and "path" in entry for entry in listing["entries"]), listing
print(f"GET /fs/list: {listing['path']} ({len(listing['entries'])} folders)")
PY

echo "OK: backend answers /models and /fs/list"
