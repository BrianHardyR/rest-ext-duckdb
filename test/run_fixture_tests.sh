#!/usr/bin/env bash
# Starts the local fixture server and runs the sqllogictests that need it.
#   test/run_fixture_tests.sh [unittest args...]      (default: test/sql/rest_ext_calls.test)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UNITTEST="${UNITTEST:-$ROOT/build/release/test/unittest}"
LOG="$(mktemp)"
python3 -I "$ROOT/test/fixtures/fixture_server.py" > "$LOG" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; rm -f "$LOG"' EXIT
for _ in $(seq 50); do grep -q '^PORT=' "$LOG" && break; sleep 0.1; done
export REST_EXT_FIXTURE_URL="http://127.0.0.1:$(sed -n 's/^PORT=//p' "$LOG")"
cd "$ROOT"
if [ "$#" -eq 0 ]; then set -- "test/sql/rest_ext_calls.test"; fi
"$UNITTEST" --test-dir . "$@"
