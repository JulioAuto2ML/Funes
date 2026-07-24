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
WORKSPACE=$(mktemp -d /tmp/funes_it_ws_XXXX)

FAILURES=0
check() {  # check <name> <haystack> <needle>
    if echo "$2" | grep -q "$3"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1 — expected '$3' in: $(echo "$2" | head -c 300)"
        FAILURES=$((FAILURES + 1))
    fi
}
check_absent() {  # check_absent <name> <haystack> <needle>
    if echo "$2" | grep -q "$3"; then
        echo "  FAIL: $1 — did not expect '$3' in: $(echo "$2" | head -c 300)"
        FAILURES=$((FAILURES + 1))
    else
        echo "  ok: $1"
    fi
}

cleanup() {
    [ -n "${FUNES_PID:-}" ] && kill "$FUNES_PID" 2>/dev/null
    [ -n "${MOCK_PID:-}" ]  && kill "$MOCK_PID" 2>/dev/null
    rm -f "$DB" "$DB-wal" "$DB-shm"
    rm -rf "$WORKSPACE"
}
trap cleanup EXIT

echo "— starting mock LLM on :$LLM_PORT"
python3 tests/mock_llm.py $LLM_PORT &
MOCK_PID=$!

echo "— starting funes on :$API_PORT"
FUNES_LLM_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_EMBED_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_DB="$DB" \
FUNES_WORKSPACE_DIR="$WORKSPACE" \
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

echo "— chat (SSE, delegate_to_agent — orchestration + persist=false)"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"please delegate-now","session":"it-session-delegate"}')
check "delegate tool_call event"   "$OUT" 'delegate_to_agent'
check "delegate tool_result event" "$OUT" 'event: tool_result'
check "delegate loop completed"    "$OUT" 'with-tool-result'

OUT=$(curl -s "$BASE/api/history?session=it-session-delegate")
check "delegated session has orchestrator turn" "$OUT" 'please delegate-now'
# The delegated sub-call (researcher's own "look something up" task/answer)
# must NOT show up as its own turn — persist=false is what keeps history to
# exactly the one exchange the user actually had.
TURN_COUNT=$(echo "$OUT" | grep -o '"role"' | wc -l)
if [ "$TURN_COUNT" -eq 2 ]; then
    echo "  ok: delegated session has exactly 2 turns, not 4"
else
    echo "  FAIL: delegated session has exactly 2 turns, not 4 — got $TURN_COUNT"
    FAILURES=$((FAILURES + 1))
fi
check_absent "delegated task text absent from history" "$OUT" 'look something up'

OUT=$(curl -s "$BASE/api/sessions")
check "sessions list ok"                  "$OUT" '"ok":true'
check "sessions list has delegate session" "$OUT" 'it-session-delegate'
check "sessions list has preview"          "$OUT" '"preview":"please delegate-now"'

echo "— chat validation"
OUT=$(curl -s -X POST "$BASE/api/chat" -d '{"session":"it-session-1"}')
check "missing message rejected" "$OUT" "'message' or 'images' is required"
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

echo "— file upload"
TEXT_FILE=$(mktemp /tmp/funes_it_upload_XXXX.txt)
echo "the quick brown fox" > "$TEXT_FILE"
OUT=$(curl -s -F "file=@$TEXT_FILE" "$BASE/api/upload")
check "text upload ok"       "$OUT" '"ok":true'
check "text upload is_text"  "$OUT" '"is_text":true'
check "text upload content"  "$OUT" 'the quick brown fox'
rm -f "$TEXT_FILE"

BIN_FILE=$(mktemp /tmp/funes_it_upload_XXXX.bin)
printf '\x00\x01\x02\x03\x04\x05\x06\x07' > "$BIN_FILE"
OUT=$(curl -s -F "file=@$BIN_FILE" "$BASE/api/upload")
check "binary upload ok"        "$OUT" '"ok":true'
check "binary upload not text"  "$OUT" '"is_text":false'
check "binary upload not image" "$OUT" '"is_image":false'
rm -f "$BIN_FILE"

PNG_FILE=$(mktemp /tmp/funes_it_upload_XXXX.png)
# 1x1 red pixel PNG.
echo "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" \
    | base64 -d > "$PNG_FILE"
OUT=$(curl -s -F "file=@$PNG_FILE" "$BASE/api/upload")
check "image upload ok"        "$OUT" '"ok":true'
check "image upload is_image"  "$OUT" '"is_image":true'
check "image upload mime type" "$OUT" '"mime_type":"image/png"'
check "image upload has data"  "$OUT" '"data":"iVBOR'
rm -f "$PNG_FILE"

echo "— chat with an image attachment"
IMG_B64="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d "{\"message\":\"what color is this\",\"session\":\"it-session-2\",\"images\":[{\"mime_type\":\"image/png\",\"data\":\"$IMG_B64\"}]}")
check "chat with image done"        "$OUT" 'event: done'
check "chat with image mock reply"  "$OUT" 'MOCK-REPLY'

OUT=$(curl -s -X POST "$BASE/api/chat" \
      -d '{"session":"it-session-2","images":[{"mime_type":"text/plain","data":"eA=="}]}')
check "non-image mime_type rejected" "$OUT" "'mime_type'"

OUT=$(curl -s -F "notfile=nope" "$BASE/api/upload")
check "upload missing file rejected" "$OUT" '"ok":false'

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "integration: all tests passed"
    exit 0
else
    echo "integration: $FAILURES test(s) FAILED (funes log: /tmp/funes_it.log)"
    exit 1
fi
