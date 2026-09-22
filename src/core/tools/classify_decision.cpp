// =============================================================================
// src/core/tools/classify_decision.cpp — classify_decision native tool
// =============================================================================
// Reflex/Jev-style "typed decision" path: given a short state and a small set
// of options, returns a calibrated probability per option in one pair of
// forward passes against the model already loaded for chat — no separate
// model, no Python, no free-text generation to parse.
//
// Two option orderings are read and averaged (Reflex's calibration trick) to
// cancel out position bias, which local instruct models show more of than
// frontier ones. Both passes use the same cache_id so llama-server lands
// them on the same slot and the second pass reuses the first pass's cached
// prefix (see LLMClient::label_probs).

#include "../agent.h"        // AgentDefaults
#include "../tools.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

constexpr size_t MAX_OPTIONS = 6;
const char LETTERS[] = "ABCDEF";

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Fallback for when the model omits an argument: pulls state/question/
// options straight out of the raw task text (ToolContext::task_text)
// instead of requiring the model to re-type a state it already received —
// see agents/classifier.yaml's delegation_notes for the exact convention
// this parses ("State: ...\nQuestion: ...\nOptions: a | b | c").
//
// Searches from the END of the text (rfind), not the start: a fully
// well-formed task's Options: line is always last, and anchoring there
// means a state that happens to *contain* the literal substring
// "\nQuestion:" or "\nOptions:" (a policy document quoting a form, say)
// doesn't split in the wrong place as readily as a forward search would.
// This is a heuristic over plain text, not a real parser — it can still be
// confused by adversarial input, which is exactly why an explicit argument
// always wins over this fallback rather than the reverse.
bool parse_task_text(const std::string& text, std::string& state,
                     std::string& question, std::vector<std::string>& options) {
    const std::string state_marker    = "State:";
    const std::string question_marker = "\nQuestion:";
    const std::string options_marker  = "\nOptions:";

    if (text.rfind(state_marker, 0) != 0) return false;  // must start with "State:"

    const size_t options_pos = text.rfind(options_marker);
    if (options_pos == std::string::npos || options_pos < state_marker.size()) return false;
    const size_t question_pos = text.rfind(question_marker, options_pos - 1);
    if (question_pos == std::string::npos || question_pos < state_marker.size()) return false;

    state    = trim(text.substr(state_marker.size(), question_pos - state_marker.size()));
    question = trim(text.substr(question_pos + question_marker.size(),
                                options_pos - (question_pos + question_marker.size())));

    std::istringstream iss(text.substr(options_pos + options_marker.size()));
    std::string token;
    while (std::getline(iss, token, '|')) {
        std::string t = trim(token);
        if (!t.empty()) options.push_back(t);
    }
    return !state.empty() && !question.empty() && options.size() >= 2;
}

std::string build_prompt(const std::string& state, const std::string& question,
                         const std::vector<std::string>& options_in_order) {
    std::ostringstream oss;
    oss << "State:\n" << state << "\n\nQuestion: " << question << "\nOptions:\n";
    for (size_t i = 0; i < options_in_order.size(); ++i)
        oss << LETTERS[i] << ") " << options_in_order[i] << "\n";
    oss << "Answer with a single letter only.\nAnswer:";
    return oss.str();
}

// Combines a pass read in the original option order with a pass read in
// reversed order (mapping the second back to the original index first), then
// renormalizes over just the candidate set. Pure and index-only so the
// position-bias-cancelling trick is exercised by the tool's own tests without
// depending on a second network call to get the math right.
std::vector<double> average_reversed(const std::vector<double>& pass1_original_order,
                                     const std::vector<double>& pass2_reversed_order) {
    const size_t n = pass1_original_order.size();
    std::vector<double> combined(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        combined[i] = pass1_original_order[i] + pass2_reversed_order[n - 1 - i];

    double sum = 0.0;
    for (double v : combined) sum += v;
    if (sum > 0.0) {
        for (double& v : combined) v /= sum;
    } else {
        // Neither pass saw any candidate label at all (e.g. n_probs too low,
        // or a tokenizer mismatch) — uniform is the honest answer, not zero.
        for (double& v : combined) v = 1.0 / static_cast<double>(n);
    }
    return combined;
}

ToolResult classify_decision_handler(const json& args, const ToolContext& ctx, LLMClient& llm) {
    const bool has_state = args.contains("state") && args["state"].is_string()
                          && !args["state"].get<std::string>().empty();
    const bool has_question = args.contains("question") && args["question"].is_string()
                              && !args["question"].get<std::string>().empty();
    const bool has_options = args.contains("options") && args["options"].is_array()
                             && args["options"].size() >= 2;

    // Explicit arguments always win — the fallback only fills in what the
    // model actually left out, so a caller not following the classifier
    // agent's task convention (a different agent, a direct tool call) is
    // completely unaffected by any of this.
    std::string parsed_state, parsed_question;
    std::vector<std::string> parsed_options;
    const bool parsed = (!has_state || !has_question || !has_options)
                       && parse_task_text(ctx.task_text, parsed_state, parsed_question, parsed_options);

    std::string state    = has_state    ? args["state"].get<std::string>()    : (parsed ? parsed_state    : "");
    std::string question = has_question ? args["question"].get<std::string>() : (parsed ? parsed_question : "");
    std::vector<std::string> options;
    if (has_options) {
        for (const auto& o : args["options"])
            if (o.is_string() && !o.get<std::string>().empty()) options.push_back(o.get<std::string>());
    } else if (parsed) {
        options = parsed_options;
    }

    if (state.empty())
        return {"Missing 'state' — not given as an argument, and couldn't be read from the task", true};
    if (question.empty())
        return {"Missing 'question' — not given as an argument, and couldn't be read from the task", true};
    if (options.size() < 2 || options.size() > MAX_OPTIONS)
        return {"'options' must resolve to 2-" + std::to_string(MAX_OPTIONS) +
                " strings, from arguments or the task", true};

    // The prompt ends in "Answer:" with no trailing space, so the model's
    // answer token includes the leading space as part of the token itself
    // (verified against yoda's Qwen3.8-9B-Q8_0: the real completion for this
    // prompt shape is " B", token id 417, not "B") — a bare letter here would
    // never match and every call would silently read back as zero everywhere.
    std::vector<std::string> letters(options.size());
    for (size_t i = 0; i < options.size(); ++i) letters[i] = std::string(" ") + LETTERS[i];

    std::vector<double> pass1(options.size(), 0.0);
    std::vector<double> pass2(options.size(), 0.0);
    try {
        auto p1 = llm.label_probs(build_prompt(state, question, options), letters, state);
        for (size_t i = 0; i < options.size(); ++i) pass1[i] = p1[i].probability;

        std::vector<std::string> reversed(options.rbegin(), options.rend());
        // Same cache_id as pass 1 — see the KV-cache reuse note on label_probs.
        auto p2 = llm.label_probs(build_prompt(state, question, reversed), letters, state);
        for (size_t i = 0; i < options.size(); ++i) pass2[i] = p2[i].probability;
    } catch (const std::exception& e) {
        return {std::string("classify_decision failed: ") + e.what(), /*error=*/true};
    }

    std::vector<double> combined = average_reversed(pass1, pass2);

    json out_options = json::array();
    size_t best = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        out_options.push_back({{"option", options[i]}, {"probability", combined[i]}});
        if (combined[i] > combined[best]) best = i;
    }
    json result = {{"options", out_options}, {"top", options[best]}};
    return {result.dump()};
}

} // namespace

void register_classify_decision_tool(ToolRegistry& reg, const AgentDefaults& defaults) {
    reg.add({
        "classify_decision",
        "Fast typed classification: given a short situation and a small set of "
        "labeled options, returns a calibrated probability for each option instead "
        "of a written answer. Use this instead of reasoning in prose when the task "
        "is really 'which bucket does this fall into' (routing, triage, spam/ham, "
        "sentiment) rather than something that needs explanation — it costs one "
        "forward pass and the confidence numbers are directly usable by other code. "
        "All three arguments are optional: if your own task already states the "
        "situation, question and options (e.g. a 'State: ... Question: ... "
        "Options: a | b' task), omit them here and this tool reads them directly "
        "from that task instead of you retyping a possibly large state.",
        {
            {"type", "object"},
            {"properties", {
                {"state",    {{"type", "string"}, {"description", "The situation or text to classify — omit if your task already states it"}}},
                {"question", {{"type", "string"}, {"description", "The decision question, e.g. 'which team should handle this?' — omit if your task already states it"}}},
                {"options",  {{"type", "array"}, {"items", {{"type", "string"}}},
                             {"minItems", 2}, {"maxItems", static_cast<int>(MAX_OPTIONS)},
                             {"description", "2-6 short option labels — omit if your task already states them"}}}
            }}
        },
        // A fresh LLMClient per call is cheap (parsed URL + connection
        // params, no persistent socket) and keeps the tool free of shared
        // mutable state across concurrent agent runs.
        [defaults](const json& args, const ToolContext& ctx) {
            LLMClient llm(defaults.llm_url, defaults.llm_api_key,
                         defaults.llm_model.empty() ? "default" : defaults.llm_model,
                         defaults.llm_provider);
            return classify_decision_handler(args, ctx, llm);
        }
    });
}
