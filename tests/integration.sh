#!/usr/bin/env bash
# =============================================================================
# tests/integration.sh — end-to-end test against a mock LLM
# =============================================================================
# Starts mock_llm.py and the funes binary on scratch ports with a scratch DB,
# then drives the whole HTTP surface with curl. No real LLM or network needed.

set -u
cd "$(dirname "$0")/.."

FUNES_BIN=${FUNES_BIN:-./bin/funes}
LLM_PORT=18080
API_PORT=18484
DB=$(mktemp -u /tmp/funes_it_XXXX.db)

FAILURES=0
check() {  # check <name> <haystack> <needle>
    if echo "$2" | grep -q "$3"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1 — expected '$3' in: $(echo "$2" | head -c 300)"
        FAILURES=$((FAILURES + 1))
    fi
}

cleanup() {
    [ -n "${FUNES_PID:-}" ] && kill "$FUNES_PID" 2>/dev/null
    [ -n "${MOCK_PID:-}" ]  && kill "$MOCK_PID" 2>/dev/null
    rm -f "$DB" "$DB-wal" "$DB-shm"
}
trap cleanup EXIT

echo "— starting mock LLM on :$LLM_PORT"
python3 tests/mock_llm.py $LLM_PORT &
MOCK_PID=$!

echo "— starting funes on :$API_PORT"
FUNES_LLM_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_EMBED_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_DB="$DB" \
FUNES_HOST=127.0.0.1 \
FUNES_PORT=$API_PORT \
"$FUNES_BIN" > /tmp/funes_it.log 2>&1 &
FUNES_PID=$!

for i in $(seq 1 50); do
    curl -sf "http://127.0.0.1:$API_PORT/api/status" > /dev/null 2>&1 && break
    sleep 0.2
done

BASE="http://127.0.0.1:$API_PORT"

echo "— status"
OUT=$(curl -s "$BASE/api/status")
check "status ok"        "$OUT" '"ok":true'
check "semantic memory"  "$OUT" '"semantic_memory":true'

echo "— agents"
OUT=$(curl -s "$BASE/api/agents")
check "funes agent listed"      "$OUT" '"funes"'
check "researcher agent listed" "$OUT" '"researcher"'

echo "— memories CRUD"
OUT=$(curl -s -X POST "$BASE/api/memories" -d '{"text":"The user is testing Funes"}')
check "memory created" "$OUT" '"ok":true'
MEM_ID=$(echo "$OUT" | grep -o '"id":[0-9]*' | cut -d: -f2)

OUT=$(curl -s "$BASE/api/memories?q=testing")
check "memory searchable" "$OUT" 'testing Funes'

OUT=$(curl -s -X POST "$BASE/api/memories" -d 'not json')
check "invalid body rejected" "$OUT" '"ok":false'

OUT=$(curl -s -X POST "$BASE/api/memories" -d '{"text":""}')
check "empty text rejected" "$OUT" '"ok":false'

echo "— chat (SSE, plain answer with memory injection)"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"hello there","session":"it-session-1"}')
check "sse done event"     "$OUT" 'event: done'
check "sse delta events"   "$OUT" 'event: delta'
check "mock reply arrived" "$OUT" 'MOCK-REPLY'
check "memories injected"  "$OUT" 'with-memories'
check "memories event"     "$OUT" 'event: memories'

echo "— chat (SSE, tool-call round trip)"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"please use-tool now","session":"it-session-1"}')
check "tool_call event"    "$OUT" 'event: tool_call'
check "tool_result event"  "$OUT" 'event: tool_result'
check "loop completed"     "$OUT" 'with-tool-result'

echo "— chat validation"
OUT=$(curl -s -X POST "$BASE/api/chat" -d '{"session":"it-session-1"}')
check "missing message rejected" "$OUT" "'message' is required"
OUT=$(curl -s -X POST "$BASE/api/chat" -d '{"message":"hi","session":"bad session!"}')
check "bad session rejected" "$OUT" 'session'
OUT=$(curl -s -X POST "$BASE/api/chat" -d '{"message":"hi","session":"s1","agent":"ghost"}')
check "unknown agent rejected" "$OUT" 'Unknown agent'

echo "— history"
OUT=$(curl -s "$BASE/api/history?session=it-session-1")
check "history has user turn"      "$OUT" 'hello there'
check "history has assistant turn" "$OUT" 'MOCK-REPLY'

echo "— auto-memory + forget"
OUT=$(curl -s "$BASE/api/memories")
check "auto-memory stored" "$OUT" '"source":"auto"'
OUT=$(curl -s -X DELETE "$BASE/api/memories/$MEM_ID")
check "memory forgotten" "$OUT" '"ok":true'
OUT=$(curl -s -X DELETE "$BASE/api/memories/$MEM_ID")
check "double-forget 404" "$OUT" '"ok":false'

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "integration: all tests passed"
    exit 0
else
    echo "integration: $FAILURES test(s) FAILED (funes log: /tmp/funes_it.log)"
    exit 1
fi
