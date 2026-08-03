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
# Scratch agents dir: the real agents/ plus fixtures the tests need. Copied
# rather than added to agents/ so test-only agents don't ship to production.
AGENTS=$(mktemp -d /tmp/funes_it_agents_XXXX)

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
    rm -rf "$WORKSPACE" "$AGENTS"
}
trap cleanup EXIT

cp agents/*.yaml "$AGENTS/"
cat > "$AGENTS/contract-tester.yaml" <<'YAML'
name: contract-tester
description: Test fixture — an agent that may not finish before write_file succeeds.
tools: [write_file]
require_tools: [write_file]
max_steps: 6
system_prompt: Write the file you are asked for.
YAML
cat > "$AGENTS/schema-tester.yaml" <<'YAML'
name: schema-tester
description: Test fixture — an agent whose final answer must be a JSON object.
tools: [recall]
max_steps: 6
system_prompt: Answer the question.
answer_schema:
  type: object
  required: [summary, sources]
  properties:
    summary: { type: string }
    sources:
      type: array
      items: { type: string }
      minItems: 1
YAML
cat > "$AGENTS/contract-schema-tester.yaml" <<'YAML'
name: contract-schema-tester
description: Test fixture — both contracts at once, to pin down their ordering.
tools: [write_file]
require_tools: [write_file]
max_steps: 8
system_prompt: Write the file you are asked for, then report.
answer_schema:
  type: object
  required: [summary, sources]
  properties:
    summary: { type: string }
    sources:
      type: array
      items: { type: string }
      minItems: 1
YAML

cat > "$AGENTS/steps-tester.yaml" <<'YAML'
name: steps-tester
description: Test fixture — an agent whose model would rather call tools than answer.
tools: [read_file]
max_steps: 3
system_prompt: Read what you need, then answer.
YAML

cat > "$AGENTS/budget-tester.yaml" <<'YAML'
name: budget-tester
description: Test fixture — an agent with a per-tool ceiling.
tools: [read_file]
max_steps: 8
tool_limits:
  read_file: 2
system_prompt: Read what you need, then answer.
YAML

# A file well over kInlineLimit (2048 bytes), for the result-store scenario.
python3 -c "open('$WORKSPACE/big.txt','w').write('BIGFILESTART' + 'm'*8000 + 'BIGFILEEND')"
# A small one, for the empty-completion salvage scenario: it stays inline, so
# the salvaged answer is the file's own text and nothing else.
printf 'SMALLFILEMARKER the small fixture\n' > "$WORKSPACE/small.txt"

echo "— starting mock LLM on :$LLM_PORT"
python3 tests/mock_llm.py $LLM_PORT &
MOCK_PID=$!

echo "— starting funes on :$API_PORT"
FUNES_LLM_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_EMBED_URL="http://127.0.0.1:$LLM_PORT" \
FUNES_DB="$DB" \
FUNES_WORKSPACE_DIR="$WORKSPACE" \
FUNES_AGENTS_DIR="$AGENTS" \
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

echo "— cron: operator schedules an agent job, then runs it immediately (run_job_now)"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"schedule-now","session":"it-session-cron","agent":"operator"}')
check "schedule_job tool_call" "$OUT" 'schedule_job'
check "run_job_now tool_call"  "$OUT" 'run_job_now'
check "cron flow completed"    "$OUT" 'MOCK-CRON-DONE'

JOB_ID=$(echo "$OUT" | grep -o 'Scheduled job #[0-9]*' | grep -o '[0-9]*' | head -1)

OUT=$(curl -s "$BASE/api/sessions")
check "cron job got its own session" "$OUT" "\"cron-$JOB_ID\""

# persist=true (unlike delegate_to_agent's false, see core/cron_runner.cpp):
# the job's task and answer show up in their own session, not the orchestrator's.
OUT=$(curl -s "$BASE/api/history?session=cron-$JOB_ID")
check "cron job session has its task"   "$OUT" 'cron-target-task'
check "cron job session has its answer" "$OUT" 'MOCK-CRON-CHILD-REPLY'

echo "— completion contract (require_tools): premature answer gets nudged, not accepted"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"contract-comply please","session":"it-session-contract","agent":"contract-tester"}')
check "nudge event emitted"      "$OUT" 'event: contract_nudge'
check "nudge names missing tool" "$OUT" 'write_file'
check "required call happened"   "$OUT" 'event: tool_result'
check "run completed after nudge" "$OUT" 'with-tool-result'
# The premature prose must not be what the caller ends up with.
check_absent "premature answer not final" "$OUT" 'event: done.*MOCK-PREMATURE-ANSWER'
if [ -f "$WORKSPACE/contract_proof.txt" ]; then
    echo "  ok: contract forced the real side effect (file exists)"
else
    echo "  FAIL: contract forced the real side effect (file exists) — missing"
    FAILURES=$((FAILURES + 1))
fi

echo "— completion contract: a model that never complies fails loudly"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"contract-refuse please","session":"it-session-contract-2","agent":"contract-tester"}')
check "contract failure reported" "$OUT" 'FAILED'
check "failure names the tool"    "$OUT" 'write_file'
# The turn that gets stored (and that a delegating agent would read) has to be
# the failure — not the model's "All done!". Asserted against history rather
# than the SSE stream, since the stream also carries recalled memories whose
# text quotes earlier successful runs.
OUT=$(curl -s "$BASE/api/history?session=it-session-contract-2")
check "stored answer is the failure"        "$OUT" 'FAILED'
check "stored answer quotes claim as unverified" "$OUT" 'unverified claim'
check_absent "no false success stored"      "$OUT" 'with-tool-result'

echo "— result store: a large tool result is stored and dereferenced, not inlined"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"big-result please","session":"it-session-results"}')
check "large result stored"        "$OUT" 'event: result_stored'
check "preview names the tool"     "$OUT" '"tool":"read_file"'
check "model dereferenced it"      "$OUT" 'read_result'
check "window reached the model"   "$OUT" 'dereferenced id='
check "window has file content"    "$OUT" 'window=BIGFILESTART'
# The 8KB payload itself must never reach the stored turn — that's the whole
# point of the feature.
OUT=$(curl -s "$BASE/api/history?session=it-session-results")
check_absent "big payload absent from history" "$OUT" 'mmmmmmmmmmmmmmmmmmmmmmmmmmmmmm'

echo "— result store: dereferencing a stored result does not store it again"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"deref-full please","session":"it-session-deref"}')
# A full window plus its trailing "N bytes remain" note lands just over the
# inline limit, so the dereference used to be stored in turn and the model got
# a new result_id instead of the text — forever. Exactly one store per run:
# the original read_file, and nothing after it.
check "window content reached the model" "$OUT" 'MOCK-DEREF-FULL got=BIGFILESTART'
if [ "$(echo "$OUT" | grep -c 'event: result_stored')" = "1" ]; then
    echo "  ok: only the original result was stored"
else
    echo "  FAIL: dereference re-stored — got $(echo "$OUT" | grep -c 'event: result_stored') stores"
    FAILURES=$((FAILURES + 1))
fi

echo "— loop bailout: gives up honestly, carrying no tool output at all"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"big-loop please","session":"it-session-salvage"}')
check "large result still stored"  "$OUT" 'event: result_stored'
OUT=$(curl -s "$BASE/api/history?session=it-session-salvage")
# A run that looped never concluded anything, so it says so. It used to open
# with "Done." and paste the last tool result — which is how a raw web dump
# reached a user as a finished newsletter.
check "bailout is an explicit failure" "$OUT" 'FAILED —'
check "names the looping tool"         "$OUT" 'read_file'
check_absent "does not claim success"  "$OUT" 'Done\.'
# Neither the preview envelope the model saw (the 2.0 regression) nor the
# payload itself (the laundering vector) may appear in an answer.
check_absent "no preview envelope shown" "$OUT" 'result_id'
check_absent "no head/tail keys shown"   "$OUT" '\\"head\\":'
check_absent "no tool payload shown"     "$OUT" 'BIGFILESTART'
if [ "$(echo "$OUT" | wc -c)" -lt 1024 ]; then
    echo "  ok: bailout answer is small"
else
    echo "  FAIL: bailout answer is small — got $(echo "$OUT" | wc -c) bytes"
    FAILURES=$((FAILURES + 1))
fi

echo "— empty completion: a failing retry still completes the turn, saying it failed"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"retry-boom please","session":"it-session-retry"}')
# The retry is best-effort — its failure must not escape run_loop and abort the
# turn (which lost the run entirely and stored nothing). But it also must not
# be papered over with the tool result: no answer means no answer.
check_absent "no error event"       "$OUT" 'event: error'
check "turn completes"              "$OUT" 'event: done'
# Assert on the *stored* turn, not the stream: the stream also carries a
# tool_result event with the file's text, so a naive check would pass anyway.
OUT=$(curl -s "$BASE/api/history?session=it-session-retry")
check "says it produced no answer"  "$OUT" 'FAILED —'
check_absent "no tool payload shown" "$OUT" 'SMALLFILEMARKER'

echo "— tool budget: past the ceiling the call is refused and the run concludes"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"budget-burn please","session":"it-session-budget","agent":"budget-tester"}')
# The ceiling forces a conclusion instead of killing the run — that is the
# whole reason for preferring it over a lower loop-detector threshold.
check "run still concludes"      "$OUT" 'MOCK-CONCLUDED from what I had'
check_absent "not a failed run"  "$OUT" 'FAILED —'
# Exactly the budget is spent: 2 calls run, the 3rd is refused. The refusal
# must also outrank the identical-args detector, which would otherwise fire
# on that same 3rd call and end the run.
if [ "$(echo "$OUT" | grep -c 'event: tool_result')" = "3" ]; then
    echo "  ok: third call refused, not executed"
else
    echo "  FAIL: third call refused — got $(echo "$OUT" | grep -c 'event: tool_result') tool results"
    FAILURES=$((FAILURES + 1))
fi
check "refusal tells it to answer" "$OUT" 'Answer now'

echo "— tool budget: a model that ignores the refusal is made to conclude anyway"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"budget-stubborn please","session":"it-session-budget2","agent":"budget-tester"}')
# The refusal message alone is only a request, and a model is free to ignore
# it — this one re-asks for the refused tool every time it is offered. The
# turn after a refusal therefore withholds tools outright, so the only move
# left is to answer. Without that, this run burns max_steps and returns
# FAILED, which is exactly what the newsletter pipeline did.
check "forced to conclude"       "$OUT" 'MOCK-FORCED-CONCLUSION'
check_absent "not a failed run"  "$OUT" 'FAILED —'
# Still exactly 3: two executed, one refused. Withholding tools must not cost
# an extra round trip of refusals before it takes effect.
if [ "$(echo "$OUT" | grep -c 'event: tool_result')" = "3" ]; then
    echo "  ok: no further calls attempted after the refusal"
else
    echo "  FAIL: expected 3 tool results, got $(echo "$OUT" | grep -c 'event: tool_result')"
    FAILURES=$((FAILURES + 1))
fi

echo "— max_steps: the final step is reserved for an answer"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"step-hog please","session":"it-session-steps","agent":"steps-tester"}')
# Running out of steps used to mean returning nothing, however much usable
# material the history held — researcher spent all 20 of its steps calling
# read_result and ended with FAILED and no sources. Nothing refuses an
# uncapped tool, so the budget mechanism cannot help here; instead the last
# step is offered no tools, which leaves synthesis as the only move.
check "answered on the last step" "$OUT" 'MOCK-LAST-STEP-ANSWER'
check_absent "not a failed run"   "$OUT" 'FAILED —'
# 3 steps, and the last is the forced answer — so 2 calls, not 3.
if [ "$(echo "$OUT" | grep -c 'event: tool_result')" = "2" ]; then
    echo "  ok: last step spent answering, not calling"
else
    echo "  FAIL: expected 2 tool results, got $(echo "$OUT" | grep -c 'event: tool_result')"
    FAILURES=$((FAILURES + 1))
fi

echo "— withheld tools: a call written as prose is not a way back in"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"xml-yielder please","session":"it-session-xml1","agent":"steps-tester"}')
# Withholding has to reach the prompt, not just the tool_choice field: a model
# that can still see the tool schema keeps reaching for it, and writes the call
# as prose when the server won't emit one natively.
check "answered once tools were gone" "$OUT" 'MOCK-YIELDED-ANSWER'
# Against the stored answer, not the raw stream: the memories injected at the
# top of a run quote earlier sessions, so a run that failed once poisons every
# later FAILED check made against the SSE.
check_absent "not a failed run" \
    "$(curl -s "$BASE/api/history?session=it-session-xml1")" 'FAILED —'

OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"xml-hog please","session":"it-session-xml2","agent":"steps-tester"}')
# And against a model that writes the XML regardless, recovery must not launder
# it into a real call — that is what undid both the budget refusal and the
# last-step reservation in production. Failing honestly is the right outcome
# here; executing the withheld call is not.
check "run fails honestly"             "$OUT" 'FAILED —'
# The stripped blob must not come back as the answer either — markup served as
# content is the other half of the same bug.
HIST=$(curl -s "$BASE/api/history?session=it-session-xml2")
check_absent "raw XML not stored as the answer" "$HIST" 'function=read_file'
if [ "$(echo "$OUT" | grep -c 'event: tool_result')" -le "2" ]; then
    echo "  ok: withheld call never executed"
else
    echo "  FAIL: withheld call ran — got $(echo "$OUT" | grep -c 'event: tool_result') tool results"
    FAILURES=$((FAILURES + 1))
fi

OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"needs-telling please","session":"it-session-xml3","agent":"steps-tester"}')
# Taking the option away silently only works on a model that notices. This one
# imitates its own transcript and keeps writing calls as prose until the loop
# states, in words, that the turn has no tools in it.
check "told, and so answered" "$OUT" 'MOCK-TOLD-SO-ANSWERED'
check_absent "not a failed run" \
    "$(curl -s "$BASE/api/history?session=it-session-xml3")" 'FAILED —'

echo "— delegation: a sub-agent that gives up reaches the caller as an error"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"delegate-boom please","session":"it-session-deleg-fail"}')
# The whole reported bug in one scenario: without this the sub-agent's failure
# is an ordinary tool result, so the parent stores it, previews it and relays
# it as content — which is how a raw web dump became a "newsletter".
check "sub-agent failure surfaced"     "$OUT" 'FAILED —'
check "parent saw a tool error"        "$OUT" 'MOCK-PARENT-SAW: error'
# An errored result skips the result store, so it stays inline and visible
# rather than becoming a preview envelope the parent has to dereference.
check_absent "failure not stored as a result" "$OUT" 'event: result_stored'

echo "— answer schema: a malformed answer gets nudged, then accepted"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"schema-comply please","session":"it-session-schema","agent":"schema-tester"}')
check "schema nudge emitted"     "$OUT" 'event: schema_nudge'
check "nudge names the problem"  "$OUT" 'sources'
OUT=$(curl -s "$BASE/api/history?session=it-session-schema")
check "stored answer is the JSON"   "$OUT" 'the mock answer'
# Canonical JSON, not the model's fenced wrapping: a downstream consumer gets
# something it can parse without knowing how the model likes to format.
check_absent "no code fence stored" "$OUT" '```'
check_absent "no preamble stored"   "$OUT" 'Sure — here it is'

echo "— answer schema: a model that never complies fails loudly"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"schema-refuse please","session":"it-session-schema-2","agent":"schema-tester"}')
check "schema failure reported" "$OUT" 'FAILED'
OUT=$(curl -s "$BASE/api/history?session=it-session-schema-2")
check "stored answer is the failure"       "$OUT" 'FAILED'
check "failure quotes the prose as unverified" "$OUT" 'MOCK-PROSE-ANSWER'

echo "— answer schema + require_tools: side effects come before formatting"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"contract-schema-both please","session":"it-session-both","agent":"contract-schema-tester"}')
check "tool contract nudged first" "$OUT" 'event: contract_nudge'
check "required call happened"     "$OUT" 'write_file'
check "schema nudged after"        "$OUT" 'event: schema_nudge'
OUT=$(curl -s "$BASE/api/history?session=it-session-both")
check "final answer satisfies both" "$OUT" 'the mock answer'
check_absent "no contract failure"  "$OUT" 'FAILED'

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
