// =============================================================================
// src/core/memory.h — Funes memory engine (SQLite + sqlite-vec)
// =============================================================================
//
// One SQLite file holds everything Funes knows:
//   memories      — long-term facts, one row each, with source and timestamp
//   vec_memories  — sqlite-vec virtual table with one embedding per memory
//   turns         — conversation history per session (short-term memory)
//   tool_results  — large tool outputs kept out of the transcript, by session
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
#include <functional>
#include <mutex>
#include <optional>
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
        std::string source;      // "user" | "auto" | "tool" | "consolidated"
        std::string created_at;  // UTC "YYYY-MM-DD HH:MM:SS"
        double      score = 0.0; // cosine similarity when recalled semantically
        int64_t     recall_count = 0;  // times this memory has been recalled
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
    // touch=false skips the recall bookkeeping consolidate() prunes on — for
    // callers that are browsing (the UI's memory search) rather than feeding
    // an agent, where counting the hit would protect memories nothing used.
    std::vector<Memory> recall(const std::string& agent, const std::string& query,
                               int k = 5, bool touch = true);

    // Newest first. agent empty = all agents.
    std::vector<Memory> list(const std::string& agent, int limit = 50, int offset = 0);

    bool    forget(int64_t id);
    int64_t count(const std::string& agent = "");

    // Embed memories that have no vector yet (up to max_items). Returns how
    // many were embedded. Safe to call from a background thread.
    size_t backfill_embeddings(size_t max_items = 256);

    // True if an embedder is configured and the last embed attempt succeeded.
    bool semantic_available() const { return embedder_ != nullptr && embedder_ok_; }

    // ── Consolidation (offline reflection pass) ───────────────────────────────
    // remember() never merges and never forgets: near-duplicates accumulate and
    // recall precision decays. consolidate() is the periodic cleanup — merge
    // near-identical memories, drop auto-captured ones nobody ever recalled.
    // No importance scores, no activation graph: just the two operations with
    // a measurable payoff and no new model-facing surface.

    // Given a cluster of near-identical memory texts, return one merged
    // sentence — or a string starting with "KEEP ALL" to leave them alone.
    // Throwing is allowed and safe: that cluster is skipped, the run goes on.
    using MergeFn = std::function<std::string(const std::vector<std::string>&)>;

    struct ConsolidationOptions {
        std::string agent;                  // empty = every agent
        double      min_similarity = 0.92;  // cosine; below this they're distinct facts
        int         prune_after_days = 30;  // age threshold for step 2; <0 disables it
        size_t      max_clusters = 20;      // cap LLM calls per run; the next run continues
    };

    struct ConsolidationReport {
        int clusters_seen = 0;  // near-duplicate clusters found
        int merged        = 0;  // memories replaced by a merged row
        int pruned        = 0;  // stale, never-recalled auto memories deleted
        int kept          = 0;  // clusters the merge function declined to merge
    };

    // Safe to call from a background thread. Each cluster is committed in its
    // own transaction, so a crash or a merge failure mid-run leaves the store
    // consistent and the next run picks up where this one stopped.
    // Without an embedder the near-duplicate step is skipped (there are no
    // vectors to cluster on) and only the prune step runs.
    // (Two overloads rather than a defaulted argument: a default argument may
    // not use a nested class's member initializers before the enclosing class
    // is complete.)
    ConsolidationReport consolidate(const MergeFn& merge, const ConsolidationOptions& opt);
    ConsolidationReport consolidate(const MergeFn& merge) {
        return consolidate(merge, ConsolidationOptions{});
    }

    // ── Tool results (session-scoped, out-of-transcript) ──────────────────────
    // Large tool outputs live here instead of in the conversation; the model
    // sees a preview plus the id (see core/result_store.h). Scoped to a chat
    // session: get_result filters by it, so an agent can never dereference
    // another session's results. Delegated sub-agents share the caller's
    // session, so handles cross the delegation boundary for free.

    int64_t store_result(const std::string& session, const std::string& agent,
                         const std::string& tool, const std::string& text);

    std::optional<std::string> get_result(const std::string& session, int64_t id);

    // Drops every result of a session (call when the session is deleted).
    int prune_results(const std::string& session);

    // Drops results older than `days` regardless of session — a startup sweep,
    // since results are only useful for the turn that produced them.
    int prune_results_older_than(int days);

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
    void touch_recalled(const std::vector<Memory>& hits);  // recall bookkeeping
    int  prune_stale(const ConsolidationOptions& opt);      // consolidate() step 2
    std::vector<Memory> recall_semantic(const std::string& agent,
                                        const std::vector<float>& qvec, int k);
    std::vector<Memory> recall_keyword(const std::string& agent,
                                       const std::string& query, int k);
};
