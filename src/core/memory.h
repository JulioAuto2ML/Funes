// =============================================================================
// src/core/memory.h — Funes memory engine (SQLite + sqlite-vec)
// =============================================================================
//
// One SQLite file holds everything Funes knows:
//   memories      — long-term facts, one row each, with source and timestamp
//   vec_memories  — sqlite-vec virtual table with one embedding per memory
//   turns         — conversation history per session (short-term memory)
//   meta          — key/value store (embedding dimension, schema version)
//
// Degrades gracefully: with no embedding endpoint (or while it is down),
// remember() stores text without a vector and recall() falls back to keyword
// search. backfill_embeddings() fills in missing vectors once the endpoint
// is available again.
//
// Thread-safe: one connection guarded by a mutex (personal-assistant scale).
// =============================================================================

#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "llm_client.h"   // EmbeddingClient, ChatMessage

struct sqlite3;

class MemoryStore {
public:
    struct Memory {
        int64_t     id = 0;
        std::string agent;
        std::string text;
        std::string source;      // "user" | "auto" | "tool"
        std::string created_at;  // UTC "YYYY-MM-DD HH:MM:SS"
        double      score = 0.0; // cosine similarity when recalled semantically
    };

    // embedder may be null — memory then works in keyword-only mode.
    // Throws std::runtime_error if the database cannot be opened/migrated.
    MemoryStore(const std::string& db_path, EmbeddingClient* embedder);
    ~MemoryStore();

    MemoryStore(const MemoryStore&)            = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // ── Long-term memory ──────────────────────────────────────────────────────

    // Store one memory. Duplicate (agent, text) pairs are ignored and the
    // existing id is returned. Embedding failures are non-fatal (stored
    // without a vector, picked up later by backfill_embeddings).
    int64_t remember(const std::string& agent, const std::string& text,
                     const std::string& source);

    // Semantic search when embeddings are available, keyword search otherwise.
    // agent filters to that agent's memories; empty = all agents.
    std::vector<Memory> recall(const std::string& agent, const std::string& query,
                               int k = 5);

    // Newest first. agent empty = all agents.
    std::vector<Memory> list(const std::string& agent, int limit = 50, int offset = 0);

    bool    forget(int64_t id);
    int64_t count(const std::string& agent = "");

    // Embed memories that have no vector yet (up to max_items). Returns how
    // many were embedded. Safe to call from a background thread.
    size_t backfill_embeddings(size_t max_items = 256);

    // True if an embedder is configured and the last embed attempt succeeded.
    bool semantic_available() const { return embedder_ != nullptr && embedder_ok_; }

    // ── Conversation history (short-term memory) ──────────────────────────────

    void append_turn(const std::string& session, const std::string& agent,
                     const std::string& role, const std::string& content);

    // Last n user/assistant turns of a session, oldest first.
    std::vector<ChatMessage> recent_turns(const std::string& session, int n = 10);

    // Total user/assistant turns stored for a session (ignores the n cap above).
    int64_t turn_count(const std::string& session);

    // Delete all but the most recent `keep` turns of a session. Used after
    // folding the older turns into the running summary below.
    void prune_turns(const std::string& session, int keep);

    // ── Rolling conversation summary (context compression) ────────────────────
    // One summary per session, replaced (not appended) each time it is
    // recompressed so it stays roughly constant size regardless of how long
    // the conversation runs.

    std::string get_summary(const std::string& session);
    void set_summary(const std::string& session, const std::string& agent,
                     const std::string& summary);

    // ── Sessions (the UI's conversation list) ─────────────────────────────────

    struct SessionSummary {
        std::string session;
        std::string last_message_at;  // UTC "YYYY-MM-DD HH:MM:SS" of the newest turn
        std::string preview;          // the session's first user message, as a title
        int64_t     turn_count = 0;   // user+assistant turns, not counting the preview lookup
    };

    // Every session that has at least one turn, newest activity first.
    std::vector<SessionSummary> list_sessions(int limit = 50);

private:
    sqlite3*         db_ = nullptr;
    std::mutex       mu_;
    EmbeddingClient* embedder_;
    bool             embedder_ok_ = true;  // false after a failed embed
    int              dim_ = 0;             // 0 until the vec table exists

    void migrate();
    void ensure_vec_table(int dim);        // creates/recreates vec_memories
    bool try_embed(const std::string& text, std::vector<float>& out);
    void insert_vector(int64_t memory_id, const std::vector<float>& vec);
    std::vector<Memory> recall_semantic(const std::string& agent,
                                        const std::vector<float>& qvec, int k);
    std::vector<Memory> recall_keyword(const std::string& agent,
                                       const std::string& query, int k);
};
