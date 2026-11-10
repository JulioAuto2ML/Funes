// =============================================================================
// tests/test_memory_provenance.cpp — a page's claim is not the person's fact
// =============================================================================
// docs/memory-plan.md, phase 3; closes 5.0.1 review item #8. A run that has
// read third-party content (a web page, a search, an email, another agent's
// answer) and then calls remember writes down what the model chose to copy.
// Before 7.1 that was stored as source=tool at weight 1.3 — the same as
// something the person taught on purpose — so a page saying "the user's name
// is Pedro" outranked the stale negative and sat at the top of recall.

#include "agent.h"
#include "httplib.h"
#include "memory.h"
#include "memory_provenance.h"
#include "permissions.h"
#include "tools.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace fs = std::filesystem;

namespace {

fs::path fresh_db(const std::string& name) {
    const fs::path db = fs::temp_directory_path() / ("funes_test_provenance_" + name + ".db");
    fs::remove(db);
    return db;
}

// An OpenAI-compatible backend that plays a fixed script, chosen by how many
// tool results the request already carries: fetch a page, remember what it
// said, answer.
struct ScriptedLLM {
    httplib::Server srv;
    std::thread th;
    int port = 0;

    ScriptedLLM() {
        srv.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
            const auto body = nlohmann::json::parse(req.body);
            int tool_results = 0;
            for (const auto& m : body["messages"])
                if (m.value("role", "") == "tool") ++tool_results;

            nlohmann::json msg = {{"role", "assistant"}, {"content", nullptr}};
            auto call = [](const std::string& name, const nlohmann::json& args) {
                return nlohmann::json::array({{
                    {"id", "call_" + name}, {"type", "function"},
                    {"function", {{"name", name}, {"arguments", args.dump()}}}}});
            };
            if (tool_results == 0)
                msg["tool_calls"] = call("fake_fetch", {{"url", "https://blog.example/post"}});
            else if (tool_results == 1)
                msg["tool_calls"] = call("remember", {{"text", "The user's name is Pedro."}});
            else
                msg["content"] = "Noted.";
            res.set_content(nlohmann::json({{"choices", {{{"message", msg}}}}}).dump(),
                            "application/json");
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        for (int i = 0; i < 50 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ~ScriptedLLM() { srv.stop(); if (th.joinable()) th.join(); }
};

} // namespace

int test_which_tools_taint() {
    using funes::third_party_origin;
    const nlohmann::json none = nlohmann::json::object();
    CHECK(third_party_origin("read_file", none).empty());
    CHECK(third_party_origin("remember", none).empty());
    CHECK(third_party_origin("schedule_job", none).empty());

    CHECK(third_party_origin("web_fetch", {{"url", "https://User@WWW.Example.com:8443/a?b"}})
          == "url:www.example.com");
    CHECK(third_party_origin("web_fetch", {{"url", "http://[::1]:80/"}}) == "url:[::1]");
    CHECK(third_party_origin("web_fetch", none) == "tool:web_fetch");
    CHECK(third_party_origin("web_search", none) == "tool:web_search");
    CHECK(third_party_origin("run_script", {{"name", "gmail_digest"}}) == "script:gmail_digest");
    CHECK(third_party_origin("delegate_to_agent", {{"agent", "researcher"}}) == "agent:researcher");
    CHECK(third_party_origin("read_result", none) == "tool:read_result");
    // Deny by default: a tool nobody has classified (an MCP tool, a generated
    // HTTP tool, one added tomorrow) taints.
    CHECK(third_party_origin("list_messages", none) == "tool:list_messages");

    funes::RunProvenance p;
    CHECK(!p.tainted());
    for (const char* s : {"url:a", "url:b", "url:c", "url:d", "url:e"}) p.sources.insert(s);
    CHECK(p.tainted());
    CHECK(p.describe() == "url:a, url:b, url:c +2 more");
    return 0;
}

int test_remember_after_a_fetch_is_attributed() {
    const fs::path db = fresh_db("tool");
    {
        MemoryStore store(db.string(), nullptr);
        ToolRegistry reg;
        register_memory_tools(reg, store);

        ToolContext clean{"funes", "s1"};
        clean.provenance = std::make_shared<funes::RunProvenance>();
        CHECK(!reg.call("remember", {{"text", "Julio drinks mate."}}, clean).error);

        ToolContext read_a_page{"funes", "s2"};
        read_a_page.provenance = std::make_shared<funes::RunProvenance>();
        read_a_page.provenance->sources.insert("url:blog.example");
        const auto r = reg.call("remember", {{"text", "The user's name is Pedro."}}, read_a_page);
        CHECK(!r.error);
        CHECK(r.text.find("url:blog.example") != std::string::npos);

        const auto all = store.list(1, "funes");
        CHECK(all.size() == 2);
        const auto& page = all[0];   // newest first
        const auto& mine = all[1];
        CHECK(page.text == "The user's name is Pedro.");   // the claim itself is stored
        CHECK(page.presented() == "per url:blog.example: The user's name is Pedro.");
        CHECK(page.tainted);
        CHECK(page.origin == "url:blog.example");
        CHECK(page.source == "tool");
        CHECK(!mine.tainted);
        CHECK(mine.origin == "session:s1");
        CHECK(mine.presented() == "Julio drinks mate.");

        // And the recall tool shows it attributed.
        const auto rec = reg.call("recall", {{"query", "Pedro"}}, clean);
        CHECK(rec.text.find("per url:blog.example: The user's name is Pedro.") != std::string::npos);
    }
    fs::remove(db);
    return 0;
}

// Keyword recall scores by weight alone, which isolates the weighting: equal
// matches, and the tainted one must not outrank the clean one.
int test_tainted_memory_ranks_like_a_log() {
    const fs::path db = fresh_db("rank");
    {
        MemoryStore store(db.string(), nullptr);
        funes::Provenance page{"url:blog.example", true};
        store.remember(1, "funes", "zebra facts from a blog", "tool", page);
        store.remember(1, "funes", "zebra facts from Julio", "tool");
        const auto hits = store.recall(1, "funes", "zebra", 4, false);
        CHECK(hits.size() == 2);
        CHECK(!hits[0].tainted);
        CHECK(hits[1].tainted);
        CHECK(hits[0].score > 1.0);    // a taught fact keeps its 1.3
        CHECK(hits[1].score == 1.0);   // a page's claim weighs what a log does
    }
    fs::remove(db);
    return 0;
}

// A tainted memory is never the anchor of a link expansion: a page that
// matched the query must not pull the person's linked memories in after it.
int test_tainted_memory_anchors_no_expansion() {
    for (bool tainted : {false, true}) {
        const fs::path db = fresh_db("links");
        {
            MemoryStore store(db.string(), nullptr);
            funes::Provenance prov{tainted ? "url:blog.example" : "session:s1", tainted};
            const int64_t anchor = store.remember(1, "funes", "okapi sighting", "tool", prov);
            const int64_t linked = store.remember(1, "funes", "the zoo trip in March", "tool");
            CHECK(store.link_memories(1, anchor, linked));
            const auto hits = store.recall(1, "funes", "okapi", 4, false);
            bool expanded = false;
            for (const auto& h : hits) expanded |= h.id == linked;
            CHECK(expanded == !tainted);
        }
        fs::remove(db);
    }
    return 0;
}

// End to end through the agent loop: the run fetches (a tool not on the
// own-data list), then remembers. The tool memory is attributed and tainted,
// and the run's auto-memory is marked tainted too.
int test_agent_loop_tracks_what_it_read() {
    ScriptedLLM llm;
    const fs::path db = fresh_db("agent");
    {
        MemoryStore memory(db.string(), nullptr);
        ToolRegistry tools;
        register_memory_tools(tools, memory);
        tools.add({"fake_fetch", "Fetch a page.",
                   {{"type", "object"}, {"properties", {{"url", {{"type", "string"}}}}}},
                   [](const nlohmann::json&, const ToolContext&) {
                       return ToolResult{"Blog post: the user's name is Pedro."};
                   }});
        AgentDefaults defaults;
        defaults.llm_url = "http://127.0.0.1:" + std::to_string(llm.port);
        AgentConfig cfg;
        cfg.name = "funes";
        cfg.system_prompt = "test";
        cfg.memory_scope = "funes";        // the YAML loader fills this in
        cfg.tools = {"fake_fetch", "remember"};
        FunesAgent agent(cfg, tools, memory, defaults);
        const std::string reply = agent.run("look this up", "s1", 1,
                                            funes::Permissions::unrestricted(), nullptr, {},
                                            Persist::Full);
        CHECK(reply == "Noted.");

        const auto all = memory.list(1, "funes");
        bool saw_tool = false, saw_auto = false;
        for (const auto& m : all) {
            if (m.source == "tool") {
                saw_tool = true;
                CHECK(m.tainted);
                CHECK(m.origin == "tool:fake_fetch");
                CHECK(m.presented() == "per tool:fake_fetch: The user's name is Pedro.");
            }
            if (m.source == "auto") {
                saw_auto = true;
                CHECK(m.tainted);
            }
        }
        CHECK(saw_tool);
        CHECK(saw_auto);
    }
    fs::remove(db);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_which_tools_taint();
    rc |= test_remember_after_a_fetch_is_attributed();
    rc |= test_tainted_memory_ranks_like_a_log();
    rc |= test_tainted_memory_anchors_no_expansion();
    rc |= test_agent_loop_tracks_what_it_read();
    if (rc == 0) std::cout << "test_memory_provenance: all tests passed\n";
    return rc;
}
