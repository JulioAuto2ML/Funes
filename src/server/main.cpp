// =============================================================================
// src/server/main.cpp — the funes binary
// =============================================================================
//
// One process serves everything: the web UI, the REST/SSE API, the agent
// runtime, and the memory engine. Start it, open http://localhost:8484,
// and talk to something that remembers you.

#include "funes_config.h"
#include "agent.h"
#include "api.h"
#include "memory.h"
#include "tools.h"
#include "tools/http_tool_runtime.h"
#include "httplib.h"
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// Resolve a data path: use `configured` if set, else try ./`relative` (cwd),
// else <exe_dir>/../`relative` (binary lives in bin/ under the project root).
static std::string resolve_dir(const std::string& configured, const std::string& relative) {
    if (!configured.empty()) return configured;
    if (fs::exists(relative)) return relative;

    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path candidate = fs::path(buf).parent_path().parent_path() / relative;
        if (fs::exists(candidate)) return candidate.string();
    }
    return relative;
}

int main() {
    funes::load_config();

    // ── configuration ─────────────────────────────────────────────────────────
    AgentDefaults defaults;
    defaults.llm_url         = funes::env("FUNES_LLM_URL", "http://localhost:8080");
    defaults.llm_api_key     = funes::env("FUNES_LLM_KEY");
    defaults.llm_provider    = funes::env("FUNES_LLM_PROVIDER", "openai");
    defaults.llm_model       = funes::env("FUNES_LLM_MODEL", "default");
    defaults.memory_turns    = funes::env_int("FUNES_MEMORY_TURNS", 10);
    defaults.memory_recall_k = funes::env_int("FUNES_MEMORY_RECALL_K", 4);
    defaults.auto_memory     = funes::env("FUNES_AUTO_MEMORY", "1") != "0";

    const std::string host          = funes::env("FUNES_HOST", "127.0.0.1");
    const int         port          = funes::env_int("FUNES_PORT", 8484);
    const std::string default_agent = funes::env("FUNES_DEFAULT_AGENT", "funes");
    const std::string agents_dir    = resolve_dir(funes::env("FUNES_AGENTS_DIR"), "agents");
    const std::string ui_dir        = resolve_dir(funes::env("FUNES_UI_DIR"), "ui");
    const std::string generated_tools_dir = resolve_dir(
        funes::env("FUNES_GENERATED_TOOLS_DIR"), "src/core/tools/generated");

    std::string db_path = funes::env("FUNES_DB");
    if (db_path.empty()) {
        const std::string home = funes::env("HOME", ".");
        std::error_code ec;
        fs::create_directories(home + "/.funes", ec);
        db_path = home + "/.funes/memory.db";
    }

    // read_file/write_file/execute_shell are confined to this directory.
    std::string workspace_dir = funes::env("FUNES_WORKSPACE_DIR");
    if (workspace_dir.empty()) {
        const std::string home = funes::env("HOME", ".");
        workspace_dir = home + "/.funes/workspace";
    }
    {
        std::error_code ec;
        fs::create_directories(workspace_dir, ec);
    }

    // With llama-server the model name is usually left as "default" — resolve
    // the real one so model-specific handling (e.g. Qwen tool-result format)
    // keys off the actual model, and the UI shows what is really running.
    if (defaults.llm_model == "default" && defaults.llm_provider == "openai") {
        std::string discovered = fetch_default_model(defaults.llm_url, defaults.llm_api_key);
        if (!discovered.empty()) {
            std::cerr << "[funes] LLM backend serves model: " << discovered << "\n";
            defaults.llm_model = discovered;
        }
    }

    // ── embedding client (optional — memory degrades to keyword search) ───────
    std::unique_ptr<EmbeddingClient> embedder;
    const std::string embed_url = funes::env("FUNES_EMBED_URL");
    if (!embed_url.empty()) {
        embedder = std::make_unique<EmbeddingClient>(
            embed_url,
            funes::env("FUNES_EMBED_KEY"),
            funes::env("FUNES_EMBED_MODEL", "default"));
    }

    // ── core services ─────────────────────────────────────────────────────────
    MemoryStore memory(db_path, embedder.get());

    ToolRegistry tools;
    register_web_tools(tools);
    register_memory_tools(tools, memory);
    register_context_tools(tools, memory, defaults);
    register_introspection_tools(tools);
    register_file_tools(tools, workspace_dir);
    register_shell_tool(tools, workspace_dir);
    funes::tools::register_all_generated_tools(tools);
    register_tool_builder(tools, generated_tools_dir);

    FunesApi api(tools, memory, defaults, agents_dir, ui_dir, default_agent, workspace_dir);

    // create_agent needs to trigger a live reload after writing a new agent
    // YAML, so it's wired up once FunesApi (which owns the agent table) exists.
    register_agent_builder(tools, agents_dir, [&api] { api.load_agents(); });

    // Embed any memories that are missing vectors (e.g. stored while the
    // embedding endpoint was down) without blocking startup.
    std::thread([&memory] {
        size_t n = memory.backfill_embeddings();
        if (n > 0)
            std::cerr << "[funes] backfilled " << n << " memory embeddings\n";
    }).detach();

    // ── HTTP server ───────────────────────────────────────────────────────────
    httplib::Server srv;
    srv.set_read_timeout(60);
    srv.set_write_timeout(1200);   // SSE chat responses can be slow on local LLMs
    api.mount(srv);

    std::cout << "\n"
              << "  Funes — an assistant that remembers\n"
              << "  ───────────────────────────────────\n"
              << "  UI:        http://" << host << ":" << port << "\n"
              << "  LLM:       " << defaults.llm_url << " (" << defaults.llm_provider
              << ", model: " << defaults.llm_model << ")\n"
              << "  Memory:    " << db_path << " (" << memory.count() << " memories, "
              << (embedder ? "semantic" : "keyword-only") << ")\n"
              << "  Agents:    " << api.agent_count() << " from " << agents_dir << "\n"
              << "  Workspace: " << workspace_dir << " (shell "
              << (funes::env("FUNES_ALLOW_SHELL", "0") == "1" ? "enabled" : "disabled") << ")\n"
              << "\n";

    signal(SIGPIPE, SIG_IGN);  // dropped SSE clients must not kill the process

    if (!srv.listen(host, port)) {
        std::cerr << "[funes] FATAL: cannot listen on " << host << ":" << port
                  << " (port in use?)\n";
        return 1;
    }
    return 0;
}
