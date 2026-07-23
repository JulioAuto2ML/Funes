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

    std::string db_path = funes::env("FUNES_DB");
    if (db_path.empty()) {
        const std::string home = funes::env("HOME", ".");
        std::error_code ec;
        fs::create_directories(home + "/.funes", ec);
        db_path = home + "/.funes/memory.db";
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

    FunesApi api(tools, memory, defaults, agents_dir, ui_dir, default_agent);

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
              << "\n";

    signal(SIGPIPE, SIG_IGN);  // dropped SSE clients must not kill the process

    if (!srv.listen(host, port)) {
        std::cerr << "[funes] FATAL: cannot listen on " << host << ":" << port
                  << " (port in use?)\n";
        return 1;
    }
    return 0;
}
