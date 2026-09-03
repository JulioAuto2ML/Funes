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
    rm -f "${COOKIE_JAR:-}" "${MEMBER_JAR:-}"
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
cat > "$AGENTS/perm-tester.yaml" <<'YAML'
name: perm-tester
description: Test fixture — offers one privileged tool and one ordinary one, so a run can report which of them survived the caller's permissions.
tools: [execute_shell, read_file]
max_steps: 4
system_prompt: Answer the probe.
YAML
cat > "$AGENTS/delegator-tester.yaml" <<'YAML'
name: delegator-tester
description: Test fixture — require_tools=[delegate_to_agent] must track the MOST RECENT call, not any call that ever succeeded.
tools: [delegate_to_agent]
require_tools: [delegate_to_agent]
max_steps: 8
system_prompt: Delegate as instructed.
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
FUNES_VISION_URL= \
FUNES_SERVICE_TOKEN= \
"$FUNES_BIN" > /tmp/funes_it.log 2>&1 &
FUNES_PID=$!

# Readiness is checked against a *public* route: /api/status needs a session,
# and there is no account yet on a scratch database.
for i in $(seq 1 50); do
    curl -sf "http://127.0.0.1:$API_PORT/api/auth/status" > /dev/null 2>&1 && break
    sleep 0.2
done

BASE="http://127.0.0.1:$API_PORT"

# ── authenticate once, for every request below ───────────────────────────────
# Funes 4.0 requires a session on everything except the four public auth
# routes. Rather than thread a cookie through ~90 call sites, the admin is
# bootstrapped here and `curl` is shadowed by a function that always sends the
# jar. `command curl` reaches the real binary, so this does not recurse.
COOKIE_JAR=$(mktemp -u /tmp/funes_it_cookies_XXXX.txt)
BOOTSTRAP=$(command curl -s -c "$COOKIE_JAR" -X POST "$BASE/api/auth/bootstrap" \
    -d '{"username":"itadmin","password":"integration-test-pw","display_name":"IT"}')
case "$BOOTSTRAP" in
    *'"ok":true'*) ;;
    *) echo "FATAL: could not bootstrap the admin account: $BOOTSTRAP"; exit 1 ;;
esac

curl() { command curl -b "$COOKIE_JAR" "$@"; }

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

# A scheduled run is a transcript nobody held, so it is kept but hidden: the
# conversation list must not show it, and ?cron=1 must be how you get to it.
# On the deployment these were 18% of the list and were being deleted by hand.
OUT=$(curl -s "$BASE/api/sessions")
check_absent "cron session hidden from the conversation list" "$OUT" "\"cron-$JOB_ID-"

OUT=$(curl -s "$BASE/api/sessions?cron=1")
# Each firing gets its own session, "cron-<id>-<epoch>", so tool budgets don't
# carry over between runs — match the prefix, then recover the full name for
# the history lookups below.
check "cron job got its own session" "$OUT" "\"cron-$JOB_ID-"
CRON_SESSION=$(echo "$OUT" | grep -o "cron-$JOB_ID-[0-9]*" | head -1)

# Persist::TurnsOnly (see core/cron_runner.cpp): the turns are kept, because
# they are the only per-run record a failed job leaves behind.
OUT=$(curl -s "$BASE/api/history?session=$CRON_SESSION")
check "cron job session has its task"   "$OUT" 'cron-target-task'
check "cron job session has its answer" "$OUT" 'MOCK-CRON-CHILD-REPLY'

# ...and the other half of TurnsOnly: no auto-memory. The one it used to write
# was phrased `User said: "[You are running as a scheduled job...]"` — a
# sentence the person never said, which then got recalled into their real
# conversations as though they had.
OUT=$(curl -s "$BASE/api/memories?limit=200")
check_absent "cron run wrote no auto-memory" "$OUT" 'You are running as a scheduled job'
check_absent "cron reply is not in memory"   "$OUT" 'MOCK-CRON-CHILD-REPLY'

# The cleanup command for databases that predate the change. Run against this
# database it must find nothing — which is the assertion that new runs are
# already clean, and that a --dry-run on a clean database is a no-op rather
# than an error.
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" cron-cleanup 2>&1)
check "cron-cleanup finds no auto-memories"  "$OUT" '0 auto-memories'
check "cron-cleanup reports the transcript"  "$OUT" 'keeping 1 scheduled-run session'
# Reporting without --apply is the safety property worth asserting, not a
# formatting detail: the command is destructive and has no undo.
check "cron-cleanup defaults to a dry run"   "$OUT" 'Dry run'
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" cron-cleanup --drop-sessions 2>&1)
check "drop-sessions is still a dry run"     "$OUT" 'Dry run'
check "drop-sessions counts the transcript"  "$OUT" '1 scheduled-run session'
OUT=$(curl -s "$BASE/api/sessions?cron=1")
check "dry run deleted nothing"              "$OUT" "\"cron-$JOB_ID-"
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" cron-cleanup --user nosuchuser 2>&1)
check "cron-cleanup rejects unknown user"    "$OUT" 'No such user'
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" cron-cleanup --bogus 2>&1)
check "cron-cleanup rejects unknown option"  "$OUT" 'Unknown option'
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" cron-cleanup --user 2>&1)
check "cron-cleanup rejects a missing value" "$OUT" 'Missing value'

echo "— completion contract (require_tools): premature answer gets nudged, not accepted"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"contract-comply please","session":"it-session-contract","agent":"contract-tester"}')
check "nudge event emitted"      "$OUT" 'event: contract_nudge'
check "nudge names missing tool" "$OUT" 'write_file'
check "required call happened"   "$OUT" 'event: tool_result'
check "run completed after nudge" "$OUT" 'with-tool-result'
# The premature prose must not be what the caller ends up with.
check_absent "premature answer not final" "$OUT" 'event: done.*MOCK-PREMATURE-ANSWER'
# 4.0: tools write inside the calling account's workspace,
# <root>/<user_id>/ — the bootstrapped admin is user 1.
if [ -f "$WORKSPACE/1/contract_proof.txt" ]; then
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

echo "— completion contract: a later failed delegate_to_agent call must not"
echo "  be excused by an earlier successful one (2026-08-04 newsletter bug)"
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"deleg-twice-then-boom please","session":"it-session-deleg-contract","agent":"delegator-tester"}')
# ai-newsletter delegates to researcher (succeeds), then to newsletter-publisher
# (fails). Without satisfied.erase on the second call's error, the first
# success permanently satisfies require_tools=[delegate_to_agent], so the
# agent answers in prose the moment the second delegation fails — no nudge,
# no third attempt, no FAILED marker for cron_runner's is_run_failure to see.
check "contract nudge fired after the failed 2nd delegation" \
      "$OUT" 'event: contract_nudge'
DELEG_CALLS=$(echo "$OUT" | grep -c 'event: tool_call')
if [ "$DELEG_CALLS" -eq 3 ]; then
    echo "  ok: forced a 3rd delegate_to_agent call, not accepted at 2"
else
    echo "  FAIL: forced a 3rd delegate_to_agent call, not accepted at 2 — got $DELEG_CALLS tool_call event(s)"
    FAILURES=$((FAILURES + 1))
fi
# The premature answer legitimately streams as a delta while it's being
# generated (before the framework has decided whether to accept it) — same
# as the contract-tester precedent above — so the real assertion is on the
# final `done` event specifically, not on the stream as a whole.
FINAL_ANSWER=$(echo "$OUT" | grep -A1 '^event: done' | tail -1)
check "eventually answered for real"    "$FINAL_ANSWER" 'MOCK-DELEGATOR-DONE'
check_absent "final answer is not the premature one" "$FINAL_ANSWER" 'premature'

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

echo "— batch upload"
BATCH_A=$(mktemp /tmp/funes_it_batch_XXXX.txt)
BATCH_B=$(mktemp /tmp/funes_it_batch_XXXX.txt)
echo "chapter one content" > "$BATCH_A"
echo "chapter two content" > "$BATCH_B"
OUT=$(curl -s -F "file=@$BATCH_A" -F "file=@$BATCH_B" -F "folder=book" "$BASE/api/upload-batch")
check "batch upload ok"       "$OUT" '"ok":true'
check "batch upload folder"   "$OUT" '"folder":"book"'
check "batch upload files"    "$OUT" '"files":'
rm -f "$BATCH_A" "$BATCH_B"

OUT=$(curl -s -F "notfile=nope" "$BASE/api/upload-batch")
check "batch upload no files rejected" "$OUT" '"ok":false'

OVERSIZE=$(mktemp /tmp/funes_it_batch_big_XXXX.bin)
dd if=/dev/zero of="$OVERSIZE" bs=1M count=6 2>/dev/null
OUT=$(curl -s -F "file=@$OVERSIZE" -F "folder=test" "$BASE/api/upload-batch")
check "batch upload skips oversize file" "$OUT" '"skipped":'
check "batch upload oversize reason"     "$OUT" 'exceeds 5 MB'
rm -f "$OVERSIZE"

echo "— workspace file listing and download"
# The batch upload above put files into the "book" folder. List them.
OUT=$(curl -s "$BASE/api/files?path=book")
check "list files ok"           "$OUT" '"ok":true'
check "list files has entries"  "$OUT" '"files":'
check "list files path"         "$OUT" '"path":"book"'

# List the workspace root — should show the "book" directory at least.
OUT=$(curl -s "$BASE/api/files")
check "list root ok"            "$OUT" '"ok":true'
check "list root has book dir"  "$OUT" '"name":"book"'

# Download one of the batch-uploaded files.
# The batch upload used sanitize_filename which strips to basename only.
FIRST_FILE=$(curl -s "$BASE/api/files?path=book" | python3 -c "import sys,json; print(json.load(sys.stdin)['files'][0]['name'])" 2>/dev/null)
if [ -n "$FIRST_FILE" ]; then
    OUT=$(curl -s "$BASE/api/files/download?path=book/$FIRST_FILE")
    check "download file content" "$OUT" 'chapter'
else
    echo "  SKIP: could not extract filename from listing"
fi

# Path traversal must be rejected.
OUT=$(curl -s "$BASE/api/files?path=../../etc")
check "list files rejects traversal" "$OUT" '"ok":false'

OUT=$(curl -s "$BASE/api/files/download?path=../../etc/passwd")
check "download rejects traversal"   "$OUT" '"ok":false'

echo "— admin-only routes (a member must not reach them)"
# A second, non-admin account. Created through the CLI against the same
# database file the running server holds open — WAL plus busy_timeout makes
# that safe, and there is no HTTP route that creates users.
MEMBER_JAR=$(mktemp -u /tmp/funes_it_member_XXXX.txt)
printf 'member-test-pw\nmember-test-pw\n' \
    | FUNES_DB="$DB" "$FUNES_BIN" useradd itmember --name "Member" > /dev/null 2>&1
OUT=$(command curl -s -c "$MEMBER_JAR" -X POST "$BASE/api/login" \
      -d '{"username":"itmember","password":"member-test-pw"}')
check "member can log in" "$OUT" '"ok":true'

mcurl() { command curl -b "$MEMBER_JAR" "$@"; }

# The reload route rebuilds every agent definition process-wide, so it is an
# admin action even though it looks read-only from the caller's side.
CODE=$(mcurl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/agents/reload" -d '')
check "member reload forbidden" "$CODE" '403'
OUT=$(mcurl -s -X POST "$BASE/api/agents/reload" -d '')
check "member reload says why" "$OUT" 'Administrator'

# The same call as the admin still works — the gate is on the role, not the
# route being broken.
OUT=$(curl -s -X POST "$BASE/api/agents/reload" -d '')
check "admin reload allowed" "$OUT" '"ok":true'

# /api/status stays open to members — they need the memory count — but the
# LLM endpoint is the operator's infrastructure, not theirs.
OUT=$(mcurl -s "$BASE/api/status")
check        "member sees status"       "$OUT" '"ok":true'
check        "member sees model name"   "$OUT" '"model"'
check_absent "member cannot see llm url" "$OUT" '"url"'
OUT=$(curl -s "$BASE/api/status")
check "admin still sees llm url" "$OUT" '"url"'

echo "— per-user permissions over HTTP (phase 4)"
# A member's schema is narrowed before the model sees it. perm-tester offers
# execute_shell and read_file; the reply says which of them actually arrived.
# An admin bypasses permissions entirely and must still see both.
OUT=$(curl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"perm-probe","session":"perm-admin","agent":"perm-tester"}')
check "admin is offered execute_shell" "$OUT" 'MOCK-SAW-SHELL+READFILE+UNTOLD'

# The default for a new member, with no permissions written at all: ordinary
# tools allowed, privileged ones denied. This is the case that needs no admin
# to have configured anything, so it is the one most likely to be relied on.
OUT=$(mcurl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"perm-probe","session":"perm-member","agent":"perm-tester"}')
check "member is denied execute_shell" "$OUT" 'MOCK-NO-SHELL+READFILE+TOLD'

# Granting it explicitly overrides the privileged-by-default denial. Also the
# only coverage `funes perms` argument parsing has.
FUNES_DB="$DB" "$FUNES_BIN" perms itmember --allow execute_shell > /dev/null 2>&1
OUT=$(mcurl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"perm-probe","session":"perm-member-2","agent":"perm-tester"}')
check "granted member is offered execute_shell" "$OUT" 'MOCK-SAW-SHELL+READFILE+UNTOLD'

# ...and denying an ordinary tool works in the other direction, proving the
# entry is the final word rather than a one-way widening.
FUNES_DB="$DB" "$FUNES_BIN" perms itmember --deny read_file > /dev/null 2>&1
OUT=$(mcurl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"perm-probe","session":"perm-member-3","agent":"perm-tester"}')
check "denied member loses read_file" "$OUT" 'MOCK-SAW-SHELL+NOREADFILE+TOLD'

echo "— per-user agent allowlist"
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itmember --agents funes,perm-tester 2>&1)
check "perms CLI reports the allowlist" "$OUT" 'perm-tester'

# The list a member is shown and the list they can actually reach must agree —
# a hidden-but-reachable agent is the IDOR shape, just on a different noun.
OUT=$(mcurl -s "$BASE/api/agents")
check        "member sees allowed agent"     "$OUT" '"perm-tester"'
check_absent "member cannot see researcher"  "$OUT" '"researcher"'
CODE=$(mcurl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/chat" \
       -d '{"message":"hello","session":"perm-member-4","agent":"researcher"}')
check "member forbidden from researcher" "$CODE" '403'

# The admin's view is unchanged by any of the above.
OUT=$(curl -s "$BASE/api/agents")
check "admin still sees researcher" "$OUT" '"researcher"'

echo "— a user can see their own permissions"
# itmember currently carries every kind of entry at once: an agent allowlist,
# a privileged tool granted, an ordinary tool denied. /api/auth/status must
# report what the runtime actually resolved, not echo the raw blob back —
# that is the whole point of the route, since the blob's "absent" means
# different things for agents and for tools.
OUT=$(mcurl -s "$BASE/api/auth/status")
check        "member sees own permissions"      "$OUT" '"permissions"'
check        "member permissions say member"    "$OUT" '"is_admin":false'
check        "member is told agents are limited" "$OUT" '"agents_restricted":true'
check        "member permissions list perm-tester" "$OUT" 'perm-tester'
check_absent "member permissions omit researcher"  "$OUT" 'researcher'
# read_file was denied explicitly; create_agent is denied by default because
# it is privileged; execute_shell was granted and must NOT appear.
check        "denied ordinary tool is reported"    "$OUT" 'read_file'
check        "denied privileged tool is reported"  "$OUT" 'create_agent'
check_absent "granted tool is not reported denied" "$OUT" 'execute_shell'

# The admin's own view: no restrictions at all, so nothing to list.
OUT=$(curl -s "$BASE/api/auth/status")
check "admin permissions say admin"       "$OUT" '"is_admin":true'
check "admin has no denied tools"         "$OUT" '"denied_tools":\[\]'
check "admin agents are unrestricted"     "$OUT" '"agents_restricted":false'

# The route is public so the UI can ask it before signing in. It must not
# describe an account to a caller who has not proved they are one.
OUT=$(curl -s "$BASE/api/auth/status" -H 'Cookie: funes_session=nonsense')
check        "anonymous status still answers" "$OUT" '"authenticated":false'
check_absent "anonymous sees no permissions"  "$OUT" '"permissions"'

echo "— an admin can see what is scheduled on the whole box"
# The admin scheduled a job earlier in this run. A member's own listing must
# be empty rather than showing it — that is the isolation — and ?all=1 must be
# refused to them rather than quietly downgraded to their own jobs, which
# would look like it worked.
OUT=$(mcurl -s "$BASE/api/jobs")
check        "member job list ok"          "$OUT" '"scope":"mine"'
check_absent "member sees no admin job"    "$OUT" 'cron-target-task'
CODE=$(mcurl -s -o /dev/null -w '%{http_code}' "$BASE/api/jobs?all=1")
check "member forbidden from the box-wide job list" "$CODE" '403'

# Give the member a job of its own. Without a second owner in the table this
# whole section proves nothing: ?all=1 and the admin's own listing would be
# the same rows, so a handler that ignored `all` entirely would still pass.
FUNES_DB="$DB" "$FUNES_BIN" perms itmember --agents any > /dev/null 2>&1
OUT=$(mcurl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"schedule-now","session":"it-member-cron","agent":"operator"}')
check "member scheduled a job" "$OUT" 'MOCK-CRON-DONE'

# The admin's own listing still shows only the admin's, and names no owner —
# in that view the owner is always the caller, so it would be noise.
OUT=$(curl -s "$BASE/api/jobs")
check        "admin sees own job"          "$OUT" 'cron-target-task'
check_absent "own listing has no owner"    "$OUT" '"owner"'
check_absent "own listing omits the member's job" "$OUT" 'itmember'

# ...and the box-wide one shows both accounts', each attributed. Both names
# have to be here: only the member's proves `all` actually widened the query.
OUT=$(curl -s "$BASE/api/jobs?all=1")
check "box-wide listing is marked"          "$OUT" '"scope":"all"'
check "box-wide listing names the admin"    "$OUT" '"owner":"itadmin"'
check "box-wide listing names the member"   "$OUT" '"owner":"itmember"'

echo "— deleting a conversation"
# Give the member a session of its own, and the admin one with the SAME name:
# the delete must take exactly one of them.
mcurl -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"member private chat","session":"doomed-session"}' > /dev/null
curl  -s -N -X POST "$BASE/api/chat" \
      -d '{"message":"admin keeps this one","session":"doomed-session"}' > /dev/null

OUT=$(mcurl -s "$BASE/api/history?session=doomed-session")
check "member session exists first" "$OUT" 'member private chat'

# Counted rather than searched: by this point the suite has stored enough
# memories that a keyword query's top-k is a ranking question, not an
# existence one.
MEMS_BEFORE=$(mcurl -s "$BASE/api/status" | grep -o '"memories":[0-9]*' | cut -d: -f2)

OUT=$(mcurl -s -X DELETE "$BASE/api/sessions/doomed-session")
check "member deletes own session" "$OUT" '"ok":true'

OUT=$(mcurl -s "$BASE/api/history?session=doomed-session")
check_absent "member session is gone" "$OUT" 'member private chat'
OUT=$(mcurl -s "$BASE/api/sessions")
check_absent "gone from member conversation list" "$OUT" 'doomed-session'

# The admin's identically-named session is untouched — this is the assertion
# that a client-supplied name can't be used to delete someone else's chat.
OUT=$(curl -s "$BASE/api/history?session=doomed-session")
check "admin session survived" "$OUT" 'admin keeps this one'

# Deleting it again, and deleting one that never existed, are the same 404 —
# so the route can't enumerate which session names another account holds.
CODE=$(mcurl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/api/sessions/doomed-session")
check "second delete is 404" "$CODE" '404'
CODE=$(mcurl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/api/sessions/never-existed")
check "unknown session is 404" "$CODE" '404'

# Memories outlive the conversation they came from — forgetting the chat must
# not forget the person.
MEMS_AFTER=$(mcurl -s "$BASE/api/status" | grep -o '"memories":[0-9]*' | cut -d: -f2)
check "memories survive a session delete" "$MEMS_AFTER" "^$MEMS_BEFORE$"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/api/sessions/bad..name")
check "invalid session name rejected" "$CODE" '404'

echo "— two users chatting at once"
# The multi-user premise rests on this and nothing had ever exercised it: no
# two accounts had chatted simultaneously. Both users are deliberately pointed
# at the SAME session name, which is the sharp case — sessions are keyed per
# user, so a partition key that leaked would show up here as one account
# reading the other's turns. Serially this is already covered; concurrently
# the writes interleave inside SQLite.
FUNES_DB="$DB" "$FUNES_BIN" perms itmember --reset > /dev/null 2>&1
SHARED_SESSION="race-session"
# Three pairs, six in-flight requests. httplib's thread pool floor is 8, so
# this fits on the smallest machine — an SSE response holds its thread for the
# whole stream, and a request that queues would look like successful
# concurrency while proving none.
CONC=3
CONC_DIR=$(mktemp -d /tmp/funes_it_conc_XXXX)

CONC_PIDS=""
CONC_START=$(date +%s%N)
for i in $(seq 1 $CONC); do
    curl -s -N -X POST "$BASE/api/chat" \
        -d "{\"message\":\"ADMINMARK-$i concurrent-probe\",\"session\":\"$SHARED_SESSION\"}" \
        > "$CONC_DIR/admin-$i.txt" 2>&1 &
    CONC_PIDS="$CONC_PIDS $!"
    mcurl -s -N -X POST "$BASE/api/chat" \
        -d "{\"message\":\"MEMBERMARK-$i concurrent-probe\",\"session\":\"$SHARED_SESSION\"}" \
        > "$CONC_DIR/member-$i.txt" 2>&1 &
    CONC_PIDS="$CONC_PIDS $!"
done
# Only these PIDs. A bare `wait` also waits on the mock LLM and the funes
# server, which are background jobs of this same shell and never exit.
# shellcheck disable=SC2086
wait $CONC_PIDS
CONC_MS=$(( ($(date +%s%N) - CONC_START) / 1000000 ))

# 1. Every request has to have completed. A locked database or a crashed
#    worker shows up as a missing `done`, not as a wrong answer.
DONE_COUNT=$(grep -l 'event: done' "$CONC_DIR"/*.txt 2>/dev/null | wc -l)
check "all concurrent chats completed" "$DONE_COUNT" "^$((CONC * 2))$"
LOCKED=$(cat "$CONC_DIR"/*.txt | grep -ci 'database is locked\|event: error' || true)
check "no lock or error events" "$LOCKED" '^0$'

# 2. Each account's history for the shared session holds its own turns and
#    only its own.
AHIST=$(curl -s "$BASE/api/history?session=$SHARED_SESSION&limit=50")
MHIST=$(mcurl -s "$BASE/api/history?session=$SHARED_SESSION&limit=50")
AMARKS=$(echo "$AHIST" | grep -o 'ADMINMARK-[0-9]' | sort -u | wc -l)
MMARKS=$(echo "$MHIST" | grep -o 'MEMBERMARK-[0-9]' | sort -u | wc -l)
check        "admin kept all its turns"      "$AMARKS" "^$CONC$"
check        "member kept all its turns"     "$MMARKS" "^$CONC$"
check_absent "admin history has no member turns"  "$AHIST" 'MEMBERMARK'
check_absent "member history has no admin turns"  "$MHIST" 'ADMINMARK'

# 3. ...and they really did overlap. Each completion stalls 0.4s in the mock,
#    so six served one at a time cannot finish in under 2.4s. Anything near
#    that means the requests queued and the isolation above was proven for
#    sequential traffic only — which the suite already covered.
SERIAL_FLOOR_MS=$((CONC * 2 * 400))
echo "  (batch took ${CONC_MS}ms; serial would need >= ${SERIAL_FLOOR_MS}ms)"
if [ "$CONC_MS" -lt "$SERIAL_FLOOR_MS" ]; then
    echo "  ok: requests overlapped rather than queueing"
else
    echo "  FAIL: requests did not overlap — ${CONC_MS}ms >= ${SERIAL_FLOOR_MS}ms serial floor"
    FAILURES=$((FAILURES + 1))
fi

rm -rf "$CONC_DIR"

echo "— an expired session cookie stops authenticating"
# Expiry has a unit test (tests/test_users.cpp) but had never been exercised
# over HTTP, which is the path that matters: the pre-routing gate and every
# handler resolve the caller through resolve_token, and a cookie that outlived
# its row is exactly the credential someone captured last month.
#
# Backdated rather than waited out — the TTL is 30 days. python3 is already a
# dependency of this suite (the mock LLM), and the server holds the database
# in WAL mode, so a second writer alongside it is safe.
MEMBER_TOKEN=$(awk '/funes_session/ {print $NF}' "$MEMBER_JAR" | tail -1)
if [ -z "$MEMBER_TOKEN" ]; then
    echo "  FAIL: could not read the member's session token from the cookie jar"
    FAILURES=$((FAILURES + 1))
else
    # Still good right now — otherwise the assertion below proves nothing.
    OUT=$(mcurl -s "$BASE/api/auth/status")
    check "member cookie authenticates before expiry" "$OUT" '"authenticated":true'

    python3 tests/expire_token.py "$DB" "$MEMBER_TOKEN"

    OUT=$(mcurl -s "$BASE/api/auth/status")
    check        "expired cookie no longer authenticates" "$OUT" '"authenticated":false'
    check_absent "expired cookie carries no identity"     "$OUT" 'itmember'

    # And the gate refuses it outright on a protected route, rather than the
    # handler answering with somebody's data.
    CODE=$(mcurl -s -o /dev/null -w '%{http_code}' "$BASE/api/memories")
    check "expired cookie is 401 on a protected route" "$CODE" '401'

    # Logging in again issues a new token, so expiry is not a lockout.
    OUT=$(command curl -s -c "$MEMBER_JAR" -X POST "$BASE/api/login" \
          -d '{"username":"itmember","password":"member-test-pw"}')
    check "member can log in again after expiry" "$OUT" '"ok":true'
    OUT=$(mcurl -s "$BASE/api/auth/status")
    check "the new cookie authenticates" "$OUT" '"authenticated":true'
fi

echo "— funes perms: bad input, and what actually gets written"
# The happy paths above only exercise `perms` as a means to an end. These are
# the arguments a person gets wrong at 23:00 on a live box, where the failure
# that matters is a command that looks like it worked.
printf 'perm-test-pw\nperm-test-pw\n' \
    | FUNES_DB="$DB" "$FUNES_BIN" useradd itperms --name "Perms" > /dev/null 2>&1

RC=0; OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms nosuchuser --reset 2>&1) || RC=$?
check "perms rejects an unknown user"       "$OUT" 'No such user'
check "perms exits nonzero for that"        "$RC"  '^1$'

RC=0; OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms 2>&1) || RC=$?
check "perms with no username prints usage" "$OUT" 'Usage'
check "perms with no username exits 2"      "$RC"  '^2$'

RC=0; OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms --agentz funes 2>&1) || RC=$?
check "perms rejects an unknown option"     "$OUT" 'Unknown option: --agentz'
check "unknown option exits 2"              "$RC"  '^2$'

# A typo in the LAST argument used to be reported as a missing value, which
# reads as though the spelling was fine.
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms --agentz 2>&1)
check "a trailing typo is still 'unknown option'" "$OUT" 'Unknown option: --agentz'

for FLAG in --agents --allow --deny; do
    RC=0; OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms "$FLAG" 2>&1) || RC=$?
    check "perms rejects $FLAG with no value" "$OUT" "Missing value for $FLAG"
    check "missing value for $FLAG exits 2"   "$RC"  '^2$'
done

# Nothing above should have written anything.
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms 2>&1)
check "a fresh member still has no blob" "$OUT" 'raw: {}'

# Now the JSON that a valid line actually stores — asserted directly, because
# every other test reads it back through Permissions::parse, which would mask
# a blob that was stored in the wrong shape but happened to parse to the same
# defaults.
FUNES_DB="$DB" "$FUNES_BIN" perms itperms --agents funes,curator --deny execute_shell \
    --allow web_search > /dev/null 2>&1
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms 2>&1)
check "agents are stored as an array"  "$OUT" '"agents":\["funes","curator"\]'
check "a denial is stored as false"    "$OUT" '"execute_shell":false'
check "a grant is stored as true"      "$OUT" '"web_search":true'
check "resolved view shows the denial" "$OUT" 'execute_shell: denied'

# A half-valid line must change nothing: the first flag is good, the second is
# not. Applying the first would be the worst outcome — a command that errored
# and still took effect.
RC=0; FUNES_DB="$DB" "$FUNES_BIN" perms itperms --agents any --nope x > /dev/null 2>&1 || RC=$?
check "half-valid line exits 2" "$RC" '^2$'
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms 2>&1)
check "half-valid line wrote nothing" "$OUT" '"agents":\["funes","curator"\]'

# --reset really empties it, and --agents any drops only the agent list.
FUNES_DB="$DB" "$FUNES_BIN" perms itperms --agents any > /dev/null 2>&1
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms 2>&1)
check        "agents any clears the list"    "$OUT" 'agents: all'
check        "agents any keeps tool entries" "$OUT" '"execute_shell":false'
FUNES_DB="$DB" "$FUNES_BIN" perms itperms --reset > /dev/null 2>&1
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itperms 2>&1)
check "reset empties the blob" "$OUT" 'raw: {}'

# An admin's entries are stored but not enforced — say so rather than let it
# look like it took effect.
OUT=$(FUNES_DB="$DB" "$FUNES_BIN" perms itadmin --deny execute_shell 2>&1)
check "perms warns when the target is an admin" "$OUT" 'stored but not enforced'
check "admin still resolves to everything"      "$OUT" 'every agent and every tool'

FUNES_DB="$DB" "$FUNES_BIN" userdel itperms > /dev/null 2>&1

rm -f "$MEMBER_JAR"

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "integration: all tests passed"
    exit 0
else
    echo "integration: $FAILURES test(s) FAILED (funes log: /tmp/funes_it.log)"
    exit 1
fi
