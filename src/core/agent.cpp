// =============================================================================
// src/core/agent.cpp — FunesAgent implementation
// =============================================================================

#include "agent.h"
#include "answer_schema.h"
#include "completion_contract.h"
#include "result_store.h"
#include "run_outcome.h"
#include "text_utils.h"
#include "tool_budget.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

// ── construction ──────────────────────────────────────────────────────────────

FunesAgent::FunesAgent(const AgentConfig& cfg, ToolRegistry& tools,
                       MemoryStore& memory, const AgentDefaults& defaults)
    : cfg_(cfg)
    , tools_(tools)
    , memory_(memory)
    , defaults_(defaults)
    , llm_(cfg.llm_url.empty()      ? defaults.llm_url      : cfg.llm_url,
           cfg.llm_api_key.empty()  ? defaults.llm_api_key  : cfg.llm_api_key,
           (cfg.model.empty() || cfg.model == "default")
               ? (defaults.llm_model.empty() ? "default" : defaults.llm_model)
               : cfg.model,
           cfg.llm_provider.empty() ? defaults.llm_provider : cfg.llm_provider)
{
    llm_.set_max_tokens(cfg_.context_limit / 4);
    llm_.set_tool_choice(cfg_.tool_choice);

    tools_schema_ = tools_.openai_schema(cfg_.tools);
    connect_mcp_servers();
}

FunesAgent::~FunesAgent() = default;

// ── external MCP servers ──────────────────────────────────────────────────────

static std::pair<std::string, int> parse_host_port(const std::string& url) {
    std::regex re(R"(^https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        throw std::runtime_error("Invalid MCP server URL: " + url);
    return {m[1].str(), m[2].matched ? std::stoi(m[2].str()) : 80};
}

json FunesAgent::mcp_tool_to_openai(const mcp::tool& t) {
    json tj = t.to_json();
    json params = tj.contains("inputSchema")
        ? tj["inputSchema"]
        : json{{"type", "object"}, {"properties", json::object()}};
    return {
        {"type", "function"},
        {"function", {
            {"name",        t.name},
            {"description", t.description},
            {"parameters",  params}
        }}
    };
}

void FunesAgent::connect_mcp_servers() {
    // Global servers (FUNES_MCP_SERVERS, semicolon-separated) + per-agent ones.
    std::vector<McpServerConfig> servers;
    std::set<std::string> seen;

    if (const char* env = std::getenv("FUNES_MCP_SERVERS")) {
        std::string s(env);
        std::size_t pos = 0;
        while (pos < s.size()) {
            std::size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            std::string url = s.substr(pos, end - pos);
            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.erase(url.begin());
            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))  url.pop_back();
            if (!url.empty() && seen.insert(url).second)
                servers.push_back({url, url});
            pos = end + 1;
        }
    }
    for (const auto& srv : cfg_.mcp_servers)
        if (seen.insert(srv.url).second)
            servers.push_back(srv);

    for (const auto& srv : servers) {
        auto [host, port] = parse_host_port(srv.url);
        auto client = std::make_unique<mcp::sse_client>(host, port);
        client->set_timeout(120);

        if (!client->initialize("funes-agent-" + cfg_.name, "1.0.0")) {
            std::cerr << "[agent:" << cfg_.name << "] WARNING: MCP server '"
                      << srv.name << "' (" << srv.url << ") unreachable — skipping.\n";
            continue;
        }

        std::vector<mcp::tool> server_tools = client->get_tools();
        const std::size_t idx = mcp_clients_.size();
        mcp_clients_.push_back(std::move(client));

        for (const auto& tool : server_tools) {
            // Native tools and earlier servers win on name collisions.
            if (tools_.has(tool.name) || mcp_tool_index_.count(tool.name)) {
                std::cerr << "[agent:" << cfg_.name << "] INFO: MCP tool '"
                          << tool.name << "' shadowed — skipping.\n";
                continue;
            }
            if (!cfg_.tools.empty()) {
                bool allowed = false;
                for (const auto& n : cfg_.tools)
                    if (n == tool.name) { allowed = true; break; }
                if (!allowed) continue;
            }
            mcp_tool_index_[tool.name] = idx;
            tools_schema_.push_back(mcp_tool_to_openai(tool));
        }
    }
}

// ── tool dispatch ─────────────────────────────────────────────────────────────

ToolResult FunesAgent::dispatch_tool(const std::string& name, const json& args,
                                     const ToolContext& ctx) {
    if (tools_.has(name))
        return tools_.call(name, args, ctx);

    auto it = mcp_tool_index_.find(name);
    if (it == mcp_tool_index_.end())
        return {"Unknown tool: " + name, /*error=*/true};

    try {
        json result = mcp_clients_[it->second]->call_tool(name, args);
        // MCP result format: {content: [{type: "text", text: "..."}]}. The
        // server is a separate process over the wire — its "text" isn't
        // guaranteed valid UTF-8, so it goes through dump_safe rather than a
        // bare .dump() (which throws on invalid UTF-8 instead of degrading).
        if (result.contains("content") && result["content"].is_array()
            && !result["content"].empty() && result["content"][0].contains("text")
            && result["content"][0]["text"].is_string())
            return {result["content"][0]["text"].get<std::string>()};
        return {funes::dump_safe(result)};
    } catch (const std::exception& e) {
        return {std::string("MCP tool '") + name + "' failed: " + e.what(), true};
    }
}

// ── run ───────────────────────────────────────────────────────────────────────

std::string FunesAgent::run(const std::string& user_message, const std::string& session,
                            const EventFn& emit, const std::vector<ImageAttachment>& images,
                            bool persist) {
    ToolContext ctx{cfg_.name, session, cfg_.workspace_dir};

    // 1. Recall relevant memories and surface them to the UI.
    std::string memory_block;
    if (defaults_.memory_recall_k > 0) {
        auto memories = memory_.recall(cfg_.name, user_message, defaults_.memory_recall_k);
        if (!memories.empty()) {
            json items = json::array();
            std::ostringstream oss;
            for (const auto& m : memories) {
                oss << "- [" << m.created_at << "] " << m.text << "\n";
                items.push_back({
                    {"id",         m.id},
                    {"text",       m.text},
                    {"source",     m.source},
                    {"score",      m.score},
                    {"created_at", m.created_at}
                });
            }
            memory_block = oss.str();
            if (emit) emit("memories", {{"items", items}});
        }
    }

    // 2. Load the rolling summary + recent turns, compressing the oldest half
    //    of the window into the summary first if it's about to crowd out the
    //    context window (automatic safety net; see context_compressor.h).
    //    Skipped for non-persisting (delegated) calls: a specialist doing one
    //    task shouldn't rewrite the shared session's summary or prune its
    //    turns as a side effect — that's the persisting caller's job.
    std::string summary = persist ? memory_.get_summary(session) : std::string();
    std::vector<ChatMessage> recent = memory_.recent_turns(session, defaults_.memory_turns);
    if (persist) {
        constexpr double kCompressTriggerFraction = 0.7;
        constexpr int    kMinKeep = 4;
        const int budget = static_cast<int>(cfg_.context_limit * kCompressTriggerFraction);
        const int estimated = estimate_tokens(cfg_.system_prompt) + estimate_tokens(summary)
                            + estimate_tokens(recent) + estimate_tokens(user_message)
                            + static_cast<int>(images.size()) * kEstimatedTokensPerImage;
        if (estimated > budget) {
            CompressOutcome result = compress_oldest_half(memory_, llm_, session, cfg_.name,
                                                           recent, summary, kMinKeep);
            if (result.compressed && emit)
                emit("context_compressed", {{"turns_folded", result.turns_folded},
                                            {"summary_preview", result.summary_preview}});
        }
    }

    // 3. Build the conversation: system (+memories+summary), recent turns, user message.
    std::vector<ChatMessage> history;
    {
        std::string sys = cfg_.system_prompt;
        if (defaults_.agent_roster &&
            (cfg_.tools.empty() ||
             std::find(cfg_.tools.begin(), cfg_.tools.end(), "delegate_to_agent") != cfg_.tools.end())) {
            std::string roster = defaults_.agent_roster(cfg_.name);
            if (!roster.empty()) {
                sys += "\n\n## Available specialist agents (delegate_to_agent)\n" + roster
                     + "Delegate to whichever of these fits the task; this list reflects "
                       "whatever agents are currently loaded.";
            }
        }
        if (!summary.empty()) {
            sys += "\n\n## Summary of earlier conversation\n" + summary;
        }
        if (!memory_block.empty()) {
            sys += "\n\n## Relevant memories from past conversations\n" + memory_block
                 + "\nUse these naturally when they help; ignore them when irrelevant.";
        }
        if (!sys.empty()) {
            ChatMessage m; m.role = "system"; m.content = sys;
            history.push_back(std::move(m));
        }
    }
    for (auto& turn : recent)
        history.push_back(std::move(turn));
    {
        ChatMessage m; m.role = "user"; m.content = user_message; m.images = images;
        history.push_back(std::move(m));
    }

    // 4. The tool loop.
    int prompt_tokens = 0;
    std::string final_text = run_loop(history, ctx, emit, prompt_tokens);

    // 5. Persist the exchange: session history always; long-term memory when
    //    auto-memory is on (source "auto" so the UI can distinguish it).
    //    Skipped for delegated calls (persist=false) — the task/answer isn't
    //    a real turn in the visible conversation, and the orchestrating
    //    call already persists its own user message and final answer.
    if (persist) {
        memory_.append_turn(session, cfg_.name, "user", user_message);
        memory_.append_turn(session, cfg_.name, "assistant", final_text);

        if (defaults_.auto_memory && !final_text.empty()) {
            std::string reply = final_text.substr(0, 300);
            if (final_text.size() > 300) reply += "…";
            try {
                memory_.remember(cfg_.name,
                                 "User said: \"" + user_message + "\" — I replied: \"" + reply + "\"",
                                 "auto");
            } catch (const std::exception& e) {
                std::cerr << "[agent:" << cfg_.name << "] auto-memory failed: "
                          << e.what() << "\n";
            }
        }
    }

    // 6. Report context usage for the UI's gauge. Real token counts come from
    //    the LLM's usage field when the backend reports one; otherwise fall
    //    back to the same char-based estimate used for the compression trigger.
    if (emit) {
        const bool estimated_usage = prompt_tokens <= 0;
        const int used = estimated_usage ? estimate_tokens(history) : prompt_tokens;
        emit("usage", {{"used", used}, {"limit", cfg_.context_limit},
                       {"estimated", estimated_usage}});
    }

    return final_text;
}

std::string FunesAgent::run_loop(std::vector<ChatMessage>& history,
                                 const ToolContext& ctx, const EventFn& emit,
                                 int& prompt_tokens_out) {
    // Exact-signature loop detection: the same (tool, args) called 3+ times
    // means a tight loop — return early with the last result. Same tool with
    // different args is legitimate.
    std::map<std::string, int> sig_counts;
    // Near-duplicate loop detection: a model that can't get a satisfying
    // result (e.g. web_search) sometimes reworks the arguments each time
    // instead of repeating them verbatim, which the exact-signature check
    // above can't catch. Cap total calls to the same tool per turn — scaled
    // to the agent's step budget so it doesn't clip legitimately
    // tool-heavy workflows, with a floor for very short budgets.
    std::map<std::string, int> name_counts;
    const int max_same_tool = std::max(cfg_.max_steps, 6);
    std::string last_tool_name;

    // Completion contract (see core/completion_contract.h): tools that must
    // have succeeded before a text answer is final. Only successful calls
    // count — an errored write_file leaves the obligation open, which is the
    // whole point.
    const funes::CompletionContract contract{cfg_.require_tools};
    std::set<std::string> satisfied;
    // Tool nudges and answer-schema nudges (see core/answer_schema.h) share one
    // budget: they're the same failure from the model's side — it tried to
    // finish and wasn't allowed to — and two independent budgets would let a
    // model alternate between the two failure modes forever.
    const bool has_schema = !cfg_.answer_schema.is_null() && !cfg_.answer_schema.empty();
    const int  nudge_budget = contract.active() ? contract.nudge_budget() : 3;
    int nudges_used = 0;
    // Set when a nudge was just injected: that one completion is forced to
    // produce a tool call, so the model can't answer the nudge with more prose.
    bool force_tool_call = false;
    // The mirror image, set when a call was refused over budget: that one
    // completion is offered no tools at all, so the only move left is to
    // answer. The refusal message asks the model to conclude, but asking is
    // not enough — on 2026-07-31 the 9B re-issued the refused web_search seven
    // times running, each refusal costing a step, until max_steps ended the
    // run with nothing. See core/tool_budget.h.
    bool force_no_tools = false;

    // Empty when the answer validates (or there is no schema); otherwise the
    // one violation to nudge on.
    auto schema_error = [&](const std::string& text) -> std::string {
        if (!has_schema) return "";
        json value;
        if (!funes::extract_answer_json(text, value))
            return "the answer contained no JSON value at all";
        return funes::validate_answer(cfg_.answer_schema, value);
    };

    // Every "we're done here" exit below routes through this, including the
    // loop-detector and max_steps bailouts: a run that stops with a required
    // call outstanding — or with an answer the agent's consumer can't parse —
    // is a failed run no matter which branch noticed first. In particular the
    // synthetic "Done. web_search completed: …" bailouts can never satisfy a
    // schema, and must not be allowed to pretend they do.
    auto finish = [&](const std::string& text) -> std::string {
        if (contract.active()) {
            const std::vector<std::string> missing = contract.missing(satisfied);
            if (!missing.empty()) return funes::contract_failure(missing, text);
        }
        const std::string err = schema_error(text);
        if (!err.empty()) return funes::schema_failure(err, text);
        return text;
    };

    DeltaFn on_delta;
    if (emit)
        on_delta = [&emit](const std::string& text) {
            emit("delta", {{"text", text}});
        };

    for (int step = 0; step < cfg_.max_steps; ++step) {
        CompletionResponse resp;
        // The last step is spent answering, not calling. Otherwise a model
        // that keeps finding one more thing to look up walks off the end of
        // its budget with a full history and nothing said: researcher made 20
        // read_result calls on 2026-07-31 and returned FAILED with no sources
        // in it. tool_limits can't cover this — an uncapped tool is never
        // refused — so the ceiling here is max_steps itself.
        //
        // This is a forced *synthesis*, not a salvage: the model still writes
        // the answer from its own history, and if it produces nothing the run
        // still fails honestly rather than relaying raw tool output.
        const bool last_step = (step + 1 == cfg_.max_steps);
        // A pending contract nudge outranks both: require_tools is a floor,
        // and a run that skips a required call is invalid however politely it
        // concluded. Better to spend the last step on the owed call and fail
        // for want of an answer than to answer with the floor unmet.
        const bool withhold = !force_tool_call && (force_no_tools || last_step);
        if (force_tool_call) llm_.set_tool_choice("required");
        else if (withhold)   llm_.set_tool_choice("none");

        // Withholding is announced as well as performed. Dropping the schema
        // decides what the server will accept; this decides what the model
        // thinks it is doing, and a model imitating a transcript full of tool
        // calls needs telling. The notice is appended for this completion only
        // — it is scaffolding, not part of the conversation, and leaving it in
        // history would have every later turn reading a claim about tools that
        // stopped being true the moment the turn ended.
        std::vector<ChatMessage> withheld_turn;
        if (withhold) {
            withheld_turn = history;
            ChatMessage notice;
            notice.role    = "user";
            notice.content = funes::withheld_notice();
            withheld_turn.push_back(std::move(notice));
        }
        const std::vector<ChatMessage>& msgs = withhold ? withheld_turn : history;

        try {
            resp = llm_.complete(msgs, tools_schema_, on_delta);
        } catch (const std::exception& e) {
            // Some OpenAI-compatible servers reject stream=true — retry once
            // without streaming before giving up.
            if (on_delta) {
                std::cerr << "[agent:" << cfg_.name << "] streaming failed ("
                          << e.what() << "), retrying without stream\n";
                resp = llm_.complete(msgs, tools_schema_);
                if (emit && !resp.content.empty())
                    emit("delta", {{"text", resp.content}});
            } else {
                llm_.set_tool_choice(cfg_.tool_choice);
                throw;
            }
        }
        if (force_tool_call || withhold) {
            llm_.set_tool_choice(cfg_.tool_choice);
            force_tool_call = false;
            force_no_tools  = false;
        }
        if (resp.prompt_tokens > 0) prompt_tokens_out = resp.prompt_tokens;

        if (resp.tool_calls.empty()) {
            // The model wants to stop. If it still owes tool calls, it doesn't
            // get to — remind it and keep looping. This has to come before the
            // empty-content retry below, which exists to *extract* a text
            // answer and would otherwise cement a premature one.
            if (contract.active()) {
                const std::vector<std::string> missing = contract.missing(satisfied);
                if (!missing.empty()) {
                    if (nudges_used++ < nudge_budget) {
                        if (!resp.content.empty()) {
                            ChatMessage asst;
                            asst.role    = "assistant";
                            asst.content = resp.content;
                            history.push_back(std::move(asst));
                        }
                        ChatMessage nudge;
                        nudge.role    = "user";
                        nudge.content = funes::contract_nudge(missing);
                        history.push_back(std::move(nudge));
                        if (emit)
                            emit("contract_nudge", {{"missing", missing},
                                                    {"attempt", nudges_used}});
                        std::cerr << "[agent:" << cfg_.name << "] premature answer at step "
                                  << step << "; still owes " << missing.size()
                                  << " required call(s), nudging (" << nudges_used
                                  << "/" << nudge_budget << ")\n";
                        force_tool_call = true;
                        continue;
                    }
                    return funes::contract_failure(missing, resp.content);
                }
            }

            // Some local models return an empty completion right after a tool
            // round trip. One follow-up call with tools disabled reliably
            // produces the text answer from the results already in history.
            // It is a withheld turn like the ones above and gets the same
            // notice — without it this retry drew another prose tool call out
            // of the 9B, and a second empty answer with it.
            std::string answer = resp.content;
            if (answer.empty() && step > 0) {
                llm_.set_tool_choice("none");
                std::vector<ChatMessage> retry_msgs = history;
                ChatMessage notice;
                notice.role    = "user";
                notice.content = funes::withheld_notice();
                retry_msgs.push_back(std::move(notice));
                CompletionResponse retry;
                try {
                    retry = llm_.complete(retry_msgs, json::array(), on_delta);
                } catch (const std::exception& e) {
                    // Best-effort by definition — this call only exists to
                    // narrate results already in history. Letting it throw
                    // would lose the run entirely; report it as a run with no
                    // answer, which is what it is.
                    std::cerr << "[agent:" << cfg_.name << "] empty-completion retry failed ("
                              << e.what() << ")\n";
                }
                llm_.set_tool_choice(cfg_.tool_choice);
                if (retry.prompt_tokens > 0) prompt_tokens_out = retry.prompt_tokens;
                if (!retry.content.empty()) answer = retry.content;
            }

            // Still nothing — either the retry came back empty too, or this is
            // step 0, where there is no tool round trip to retry against and
            // an empty completion used to be returned verbatim as the answer.
            //
            // This used to fall back to the last tool result, which reads as
            // content: a researcher's raw web_search dump once travelled up
            // two delegation hops and was served as a finished newsletter. Say
            // so instead — and say it loudly, because the silence here is what
            // made that take a database dig to diagnose. Under a schema it
            // falls through to the validation below, which fails on its own
            // terms and reports what was wrong with the shape.
            if (answer.empty() && !has_schema) {
                std::cerr << "[agent:" << cfg_.name << "] no answer at step " << step
                          << "; giving up without one\n";
                return funes::empty_answer_failure(last_tool_name);
            }

            // The answer schema (core/answer_schema.h). Unlike a tool nudge
            // this doesn't force a tool call — we want text from the model,
            // just correct text.
            if (has_schema) {
                const std::string err = schema_error(answer);
                if (!err.empty()) {
                    if (nudges_used++ < nudge_budget) {
                        if (!answer.empty()) {
                            ChatMessage asst;
                            asst.role    = "assistant";
                            asst.content = answer;
                            history.push_back(std::move(asst));
                        }
                        ChatMessage nudge;
                        nudge.role    = "user";
                        nudge.content = funes::schema_nudge(err, cfg_.answer_schema);
                        history.push_back(std::move(nudge));
                        if (emit)
                            emit("schema_nudge", {{"error", err}, {"attempt", nudges_used}});
                        std::cerr << "[agent:" << cfg_.name << "] answer rejected at step "
                                  << step << ": " << err << "; nudging (" << nudges_used
                                  << "/" << nudge_budget << ")\n";
                        continue;
                    }
                    return funes::schema_failure(err, answer);
                }
                // Hand the consumer the canonical form, not whatever wrapping
                // the model chose: a downstream agent gets parseable JSON even
                // when the model fenced it or prefixed a sentence.
                json value;
                funes::extract_answer_json(answer, value);
                return funes::dump_safe(value);
            }
            return answer;
        }

        // Assistant message with tool_calls must precede each tool result.
        json tc_arr = json::array();
        for (const auto& tc : resp.tool_calls) {
            tc_arr.push_back({
                {"id",   tc.id},
                {"type", "function"},
                {"function", {
                    {"name",      tc.name},
                    {"arguments", tc.arguments.dump()}
                }}
            });
        }
        ChatMessage asst_msg;
        asst_msg.role       = "assistant";
        asst_msg.content    = resp.content;
        asst_msg.tool_calls = tc_arr;
        history.push_back(std::move(asst_msg));

        for (const auto& tc : resp.tool_calls) {
            const std::string call_sig = tc.name + "|" + tc.arguments.dump();
            const int sig_calls  = ++sig_counts[call_sig];
            const int name_calls = ++name_counts[tc.name];

            // A declared ceiling (core/tool_budget.h) outranks the loop
            // detectors below. Both stop the same behaviour, but a refusal is
            // recoverable — the model reads it and concludes from what it has
            // — where the detectors end the run with no answer at all. So when
            // the agent's author has said how many calls are reasonable, that
            // answer wins; max_steps remains the backstop if the model spends
            // the rest of its budget asking anyway.
            const bool refused = funes::over_budget(cfg_.tool_limits, tc.name, name_calls);

            if (!refused) {
                // Neither of these used to say so: both opened with "Done." and
                // pasted the last tool result, which is a claim of success that
                // the detector firing already disproves.
                if (sig_calls >= 3) {
                    std::cerr << "[agent:" << cfg_.name << "] loop detected: " << tc.name
                              << " repeated with identical arguments\n";
                    return finish(funes::loop_failure(tc.name, 3));
                }
                if (name_calls > max_same_tool) {
                    std::cerr << "[agent:" << cfg_.name << "] loop detected: " << tc.name
                              << " called " << max_same_tool << "+ times without progress\n";
                    return finish(funes::loop_failure(tc.name, max_same_tool));
                }
            }

            if (emit) emit("tool_call", {{"name", tc.name}, {"args", tc.arguments}});

            if (refused) {
                std::cerr << "[agent:" << cfg_.name << "] " << tc.name
                          << " refused: budget of " << cfg_.tool_limits.at(tc.name)
                          << " call(s) used up\n";
                // Withhold tools on the next completion. Set here rather than
                // where the refusal is composed, so a batch of calls in one
                // step needs only a single forced turn to settle.
                force_no_tools = true;
            }

            ToolResult result = refused
                ? ToolResult{funes::budget_message(tc.name, cfg_.tool_limits.at(tc.name)), true}
                : dispatch_tool(tc.name, tc.arguments, ctx);

            if (emit) {
                // Truncate on bytes first (result.text may be arbitrarily
                // long native-tool or MCP output), then trim to a clean
                // UTF-8 boundary so the preview itself is always safe to
                // dump — a bare substr() can split a multi-byte character.
                std::string preview = result.text;
                const bool was_truncated = preview.size() > 200;
                if (was_truncated) funes::truncate_utf8_safe(preview, 200);
                if (was_truncated) preview += "…";
                emit("tool_result", {{"name", tc.name}, {"preview", preview},
                                     {"error", result.error}});
            }

            last_tool_name   = tc.name;
            if (!result.error) satisfied.insert(tc.name);

            // Large results go to the store and the transcript carries a
            // preview instead (see core/result_store.h). Errors are left alone:
            // they're short, and their text is what the model needs to recover.
            std::string tool_text = result.text;
            if (!result.error && funes::exceeds_inline_limit(tool_text)) {
                try {
                    const int64_t rid = memory_.store_result(ctx.session, cfg_.name,
                                                             tc.name, tool_text);
                    tool_text = funes::result_preview(rid, tc.name, result.text);
                    if (emit) emit("result_stored", {{"result_id", rid},
                                                     {"tool", tc.name},
                                                     {"bytes", result.text.size()}});
                } catch (const std::exception& e) {
                    // Storage is an optimisation; failing it must not lose the
                    // result the model is waiting for.
                    std::cerr << "[agent:" << cfg_.name << "] result store failed for "
                              << tc.name << ": " << e.what() << " — inlining\n";
                }
            }
            ChatMessage tool_msg;
            tool_msg.role         = "tool";
            tool_msg.content      = result.error
                ? "{\"error\": " + funes::dump_safe(json(result.text)) + "}"
                : tool_text;
            tool_msg.tool_call_id = tc.id;
            tool_msg.name         = tc.name;
            tool_msg.images       = result.images;
            history.push_back(std::move(tool_msg));
        }
    }

    std::cerr << "[agent:" << cfg_.name << "] exhausted max_steps=" << cfg_.max_steps
              << " without a final answer\n";
    return finish(funes::max_steps_failure(cfg_.max_steps, last_tool_name));
}
