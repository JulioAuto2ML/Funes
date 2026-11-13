// =============================================================================
// src/core/chat_runs.h — the chat runs in flight, and how one is stopped
// =============================================================================
//
// Until 7.16 a run was stopped by its connection: the SSE writer's first
// failed write set the cancel flag, and the run ended at its next step with
// "(stopped by user)" saved as the answer. That conflated two things. The ■
// button closes the connection, but so does a phone locking its screen or
// switching apps — iOS suspends a page's sockets within seconds — and so does
// a 4G link dropping a stream that said nothing while the model thought. On
// 2026-11-13 a question asked from the phone was killed that way at step 0,
// and the person came back to an answer they had not asked to stop.
//
// So a dropped connection now only stops the *streaming*; the run finishes
// and persists its turns, and the client asks `running()` until it can read
// the answer from /api/history. Stopping is an explicit request
// (POST /api/chat/stop), which sets the flags registered here for that
// account's conversation. Keyed by (user, session): a session id is only
// meaningful inside one account, so another account naming it reaches
// nothing.

#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace funes {

class ChatRuns {
public:
    using Flag = std::shared_ptr<std::atomic<bool>>;

    // A new run in `session`; the returned flag is what FunesAgent checks
    // between steps (cancel_flag()). More than one run per conversation is
    // possible — a second message sent while the first is still working —
    // and stop() stops them all.
    Flag start(int64_t user_id, const std::string& session) {
        auto flag = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mu_);
        runs_[{user_id, session}].push_back(flag);
        return flag;
    }

    void finish(int64_t user_id, const std::string& session, const Flag& flag) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = runs_.find({user_id, session});
        if (it == runs_.end()) return;
        auto& v = it->second;
        for (auto f = v.begin(); f != v.end(); ++f)
            if (*f == flag) { v.erase(f); break; }
        if (v.empty()) runs_.erase(it);
    }

    // True when something was running to stop.
    bool stop(int64_t user_id, const std::string& session) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = runs_.find({user_id, session});
        if (it == runs_.end()) return false;
        for (auto& f : it->second) f->store(true, std::memory_order_relaxed);
        return true;
    }

    bool running(int64_t user_id, const std::string& session) const {
        std::lock_guard<std::mutex> lock(mu_);
        return runs_.count({user_id, session}) > 0;
    }

private:
    mutable std::mutex mu_;
    std::map<std::pair<int64_t, std::string>, std::vector<Flag>> runs_;
};

} // namespace funes
