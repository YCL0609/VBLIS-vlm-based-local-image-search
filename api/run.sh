#!/usr/bin/env bash
# Convenience wrapper: run the backend without Electron.
#   ./run.sh                 -> http://127.0.0.1:8000
#   ./run.sh --port 8011     -> any run.py flag
# Uses the local .venv when present, else the system python3.
set -euo pipefail
cd "$(dirname "$0")"

if [ -x .venv/bin/python ]; then
  PY=.venv/bin/python
elif command -v python3 >/dev/null 2>&1; then
  PY=python3
else
  PY=python
fi

exec "$PY" run.py "$@"
