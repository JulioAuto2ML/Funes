// =============================================================================
// src/core/tools/delegation.cpp — delegate_to_agent native tool
// =============================================================================
// Lets the orchestrating agent (funes) hand a task to a specialist persona
// (operator, researcher, …) and get back a single answer, the same way it
// would call any other tool. The specialist runs black-box: its own
// tool_call/tool_result/delta events are not streamed to the UI, and its
// task/answer are never persisted as a separate turn (see FunesAgent::run's
// `persist` parameter) — only the orchestrator's own turn shows up in the
// visible conversation and the session list.
//
// Only funes gets this tool (see agents/funes.yaml) — specialists are leaf
// workers by convention, so there's normally no recursion. The depth guard
// below is defense in depth in case a future custom agent (built via
// agent-builder) is ever given this tool too.
//
// Delegation by reference comes for free from the result store: the sub-agent
// runs in the *caller's* session, and its answer is an ordinary ToolResult, so
// a large one crosses the same kInlineLimit threshold as any other tool output
// (run_loop, core/result_store.h) and the orchestrator gets a preview plus a
// result_id it can dereference with read_result. Nothing to plumb here.

#include "../agent.h"
#include "../tools.h"
#include "../run_outcome.h"
#include <sstream>

namespace {

constexpr int kMaxDelegationDepth = 2;
thread_local int g_delegation_depth = 0;

ToolResult delegate_handler(ToolRegistry& reg, MemoryStore& memory, const AgentDefaults& defaults,
                            const std::function<AgentConfig(const std::string&)>& find_agent,
                            const std::function<std::vector<std::string>()>& list_agent_names,
                            const json& args, const ToolContext& ctx) {
    if (!args.contains("agent") || !args["agent"].is_string() || args["agent"].get<std::string>().empty())
        return {"Missing 'agent' argument", true};
    if (!args.contains("task") || !args["task"].is_string() || args["task"].get<std::string>().empty())
        return {"Missing 'task' argument", true};

    const std::string agent_name = args["agent"].get<std::string>();
    const std::string task = args["task"].get<std::string>();

    if (agent_name == ctx.agent)
        return {"Refusing to delegate to yourself ('" + agent_name + "').", true};

    AgentConfig target = find_agent(agent_name);
    if (target.name.empty()) {
        std::ostringstream oss;
        oss << "Unknown agent '" << agent_name << "'. Available: ";
        auto names = list_agent_names();
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) oss << ", ";
            oss << names[i];
        }
        return {oss.str(), true};
    }

    if (g_delegation_depth >= kMaxDelegationDepth)
        return {"Delegation depth limit reached — refusing to delegate further from '" +
                agent_name + "'.", true};

    struct DepthGuard {
        ~DepthGuard() { --g_delegation_depth; }
    } depth_guard;
    ++g_delegation_depth;

    try {
        FunesAgent sub(target, reg, memory, defaults);
        // No emit (black-box: sub-agent's own tool calls aren't streamed),
        // no images (this is a text task description, not the user's turn),
        // persist=false (see file header).
        std::string result = sub.run(task, ctx.session, nullptr, {}, /*persist=*/false);
        // A sub-agent that gave up returns a string like any other, so without
        // this the caller stores it, previews it, and relays it as content —
        // which is how a raw web_search dump once climbed two delegation hops
        // and was served to the user as a finished newsletter. Marking it as a
        // tool error also keeps it out of the result store, so it stays inline
        // and visible instead of turning into a preview envelope.
        return {result, funes::is_run_failure(result)};
    } catch (const std::exception& e) {
        return {std::string("Delegation to '") + agent_name + "' failed: " + e.what(), true};
    }
}

} // namespace

void register_delegation_tool(ToolRegistry& reg, MemoryStore& memory, const AgentDefaults& defaults,
                              std::function<AgentConfig(const std::string&)> find_agent,
                              std::function<std::vector<std::string>()> list_agent_names) {
    reg.add({
        "delegate_to_agent",
        "Hand a task to a specialist agent and get back its final answer. Use this instead of "
        "telling the user to switch agents themselves — you stay in the conversation and relay "
        "the result in your own voice. The specialist's own steps aren't shown to the user, so "
        "briefly mention what you had it do if it's not obvious from your answer. A long answer "
        "comes back as a preview with a result_id — call read_result with it to read the rest.",
        {
            {"type", "object"},
            {"properties", {
                {"agent", {{"type", "string"},
                          {"description", "Name of the specialist agent, e.g. operator, researcher, tool-builder, agent-builder"}}},
                {"task",  {{"type", "string"},
                          {"description", "A clear, self-contained description of what the specialist should do"}}}
            }},
            {"required", json::array({"agent", "task"})}
        },
        [&reg, &memory, &defaults, find_agent, list_agent_names](const json& args, const ToolContext& ctx) {
            return delegate_handler(reg, memory, defaults, find_agent, list_agent_names, args, ctx);
        }
    });
}
