// =============================================================================
// src/core/proposals.h — tool calls waiting for the person (OS plan, phase 6)
// =============================================================================
// What the gate in FunesAgent::dispatch_tool stores instead of running a host
// or external call from a tainted run (effects.h): the agent, the tool, its
// exact arguments, the session, and what the run had read. /api/chat runs it
// when the owner answers "aprobar <id>", or drops it on "rechazar <id>".
//
// Like every other store, ownership is in the SQL: get/decide carry
// `AND user_id = ?`, so another account's proposal and no proposal are the
// same answer. A proposal is decided once — decide() moves it out of
// `pending` in the same UPDATE that checks it is pending, so two "aprobar 4"
// arriving together run it once. One older than kTtlSeconds can no longer be
// approved: the person answering a day later is answering a different context.
// =============================================================================

#pragma once
#include "json.hpp"
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace funes {

struct Proposal {
    int64_t        id = 0;
    int64_t        user_id = 0;
    std::string    agent;
    std::string    tool;
    nlohmann::json args = nlohmann::json::object();
    std::string    session;
    std::string    origin;      // what the run had read (RunProvenance::describe)
    std::string    effect;      // effect_name()
    std::string    status;      // pending | approved | rejected | done | failed
    std::string    result;      // the call's output once run, capped
    std::string    stage;       // staging.h: the call already ran against a copy; approving commits it
    int64_t        created_at = 0;
    int64_t        decided_at = 0;
};

class ProposalStore {
public:
    static constexpr int64_t kTtlSeconds = 24 * 3600;

    explicit ProposalStore(const std::string& db_path);
    ~ProposalStore();
    ProposalStore(const ProposalStore&) = delete;
    ProposalStore& operator=(const ProposalStore&) = delete;

    int64_t create(int64_t user_id, const std::string& agent, const std::string& tool,
                   const nlohmann::json& args, const std::string& session,
                   const std::string& origin, const std::string& effect,
                   const std::string& stage = "");

    std::optional<Proposal> get(int64_t user_id, int64_t id);

    enum class Decision { Ok, NotFound, AlreadyDecided, Expired };
    // pending → approved | rejected. Only Ok means the caller may act on it.
    Decision decide(int64_t user_id, int64_t id, bool approve);

    // approved → done | failed, with what the call returned.
    void finish(int64_t user_id, int64_t id, bool ok, const std::string& result);

    // The owner's pending proposals, newest first, expired ones excluded.
    std::vector<Proposal> pending(int64_t user_id, int limit = 20);

    // The stages of proposals still waiting (pending, not expired) — what
    // staging::prune must keep.
    std::set<std::string> live_stages();

    // For tests: the clock decide() and pending() compare against.
    void set_now_for_test(int64_t now) { now_override_ = now; }

private:
    int64_t now() const;
    sqlite3*   db_ = nullptr;
    std::mutex mu_;
    int64_t    now_override_ = 0;
};

} // namespace funes
