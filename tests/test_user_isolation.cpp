// =============================================================================
// tests/test_user_isolation.cpp — one account must never see another's data
// =============================================================================
// Phase 2 of docs/dev-plan-users-permissions.md. These are the assertions that
// turn into a privacy breach rather than a bug if they regress, so they get
// their own file instead of a corner of test_memory.cpp.
//
// Two properties are being checked, and they are different:
//
//   Isolation — user B's query never returns user A's row. A leak.
//   Fidelity  — user B's query returns everything of B's that it should, even
//               when A's pool is much larger. Not a leak, but the failure mode
//               the vec0 partition key exists to prevent: before it, KNN ran
//               across every user's vectors and was filtered afterwards, so a
//               busy account could crowd a quiet one out of its own top-k and
//               recall would just quietly get worse as accounts were added.

#include "memory.h"
#include "sqlite3.h"
#include "tools.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

// Same letter-frequency embedder test_memory.cpp uses: deterministic cosine
// similarity with no model involved.
class FakeEmbedder : public EmbeddingClient {
public:
    FakeEmbedder() : EmbeddingClient("http://unused:1", "", "fake") {}
    std::vector<float> embed(const std::string& text) override {
        std::vector<float> v(26, 0.0f);
        for (char c : text) {
            if (c >= 'a' && c <= 'z') v[c - 'a'] += 1.0f;
            if (c >= 'A' && c <= 'Z') v[c - 'A'] += 1.0f;
        }
        float norm = 0;
        for (float x : v) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 0) for (float& x : v) x /= norm;
        else v[0] = 1.0f;
        return v;
    }
};

static std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_iso_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

constexpr int64_t ALICE = 1;
constexpr int64_t BOB   = 2;

int test_memories_are_isolated() {
    MemoryStore store(temp_db("mem"), nullptr);   // keyword mode

    store.remember(ALICE, "funes", "Alice's bank PIN is 4321", "user");
    store.remember(BOB,   "funes", "Bob likes hiking", "user");

    // Recall
    auto bob_hits = store.recall(BOB, "funes", "bank PIN", 10);
    for (const auto& m : bob_hits) CHECK(m.text.find("Alice") == std::string::npos);

    auto alice_hits = store.recall(ALICE, "funes", "bank PIN", 10);
    CHECK(alice_hits.size() == 1);
    CHECK(alice_hits[0].user_id == ALICE);

    // List
    auto bob_list = store.list(BOB, "", 100, 0);
    CHECK(bob_list.size() == 1);
    CHECK(bob_list[0].text == "Bob likes hiking");

    // Count
    CHECK(store.count(ALICE) == 1);
    CHECK(store.count(BOB) == 1);
    return 0;
}

int test_same_text_is_two_memories() {
    // The pre-4.0 constraint was UNIQUE(agent, text): the second user to store
    // a fact would silently get the first user's row id back and store
    // nothing. It has to be UNIQUE(user_id, agent, text).
    MemoryStore store(temp_db("dup"), nullptr);

    int64_t a = store.remember(ALICE, "funes", "the sky is blue", "user");
    int64_t b = store.remember(BOB,   "funes", "the sky is blue", "user");
    CHECK(a != 0 && b != 0);
    CHECK(a != b);
    CHECK(store.count(ALICE) == 1);
    CHECK(store.count(BOB) == 1);

    // Within one user it still deduplicates.
    int64_t again = store.remember(ALICE, "funes", "the sky is blue", "user");
    CHECK(again == a);
    CHECK(store.count(ALICE) == 1);
    return 0;
}

int test_forget_cannot_cross_users() {
    // Memory ids are sequential integers exposed as DELETE /api/memories/<id>,
    // so without an ownership predicate this is an IDOR: count upwards and
    // delete someone else's memories.
    MemoryStore store(temp_db("forget"), nullptr);

    int64_t alice_mem = store.remember(ALICE, "funes", "Alice's private note", "user");

    CHECK(!store.forget(BOB, alice_mem));       // refused
    CHECK(store.count(ALICE) == 1);             // and nothing was deleted
    auto still = store.list(ALICE, "", 10, 0);
    CHECK(still.size() == 1);

    CHECK(store.forget(ALICE, alice_mem));      // the owner still can
    CHECK(store.count(ALICE) == 0);

    // Deleting a nonexistent id is the same false, so the endpoint can't be
    // used to probe which ids exist.
    CHECK(!store.forget(ALICE, 99999));
    return 0;
}

int test_turns_and_sessions_are_isolated() {
    // Session ids are client-supplied strings, so two users picking the same
    // one is entirely possible — by accident or on purpose.
    MemoryStore store(temp_db("turns"), nullptr);
    const std::string shared = "s-collision";

    store.append_turn(ALICE, shared, "funes", "user", "Alice's secret question");
    store.append_turn(ALICE, shared, "funes", "assistant", "Alice's answer");
    store.append_turn(BOB,   shared, "funes", "user", "Bob's question");

    auto bob_turns = store.recent_turns(BOB, shared, 50);
    CHECK(bob_turns.size() == 1);
    CHECK(bob_turns[0].content == "Bob's question");

    auto alice_turns = store.recent_turns(ALICE, shared, 50);
    CHECK(alice_turns.size() == 2);

    CHECK(store.turn_count(ALICE, shared) == 2);
    CHECK(store.turn_count(BOB, shared) == 1);

    // The conversation list must not show a session's existence across users
    // with the other user's first message as the preview.
    auto bob_sessions = store.list_sessions(BOB, 50);
    CHECK(bob_sessions.size() == 1);
    CHECK(bob_sessions[0].preview == "Bob's question");
    CHECK(bob_sessions[0].turn_count == 1);
    return 0;
}

int test_deleting_a_session_cannot_cross_users() {
    // Deleting a conversation is the one destructive thing a member can do to
    // their own data, and session ids are client-supplied — so "delete
    // s-collision" from Alice must not take Bob's session of the same name
    // with it. Same shape as forget()'s ownership predicate: the user_id is on
    // the DELETE itself, not on a check before it.
    MemoryStore store(temp_db("delsession"), nullptr);
    const std::string shared = "s-collision";

    store.append_turn(ALICE, shared, "funes", "user", "Alice's secret question");
    store.append_turn(ALICE, shared, "funes", "assistant", "Alice's answer");
    store.set_summary(ALICE, shared, "funes", "Alice's running summary");
    store.store_result(ALICE, shared, "funes", "web_fetch", "Alice's fetched page");

    store.append_turn(BOB, shared, "funes", "user", "Bob's question");
    store.set_summary(BOB, shared, "funes", "Bob's running summary");
    store.store_result(BOB, shared, "funes", "web_fetch", "Bob's fetched page");

    // Everything of Alice's goes; nothing of Bob's does.
    CHECK(store.delete_session(ALICE, shared) == 2);   // turns removed
    CHECK(store.turn_count(ALICE, shared) == 0);
    CHECK(store.get_summary(ALICE, shared).empty());
    CHECK(store.list_sessions(ALICE, 50).empty());

    CHECK(store.turn_count(BOB, shared) == 1);
    CHECK(store.get_summary(BOB, shared) == "Bob's running summary");
    auto bob_sessions = store.list_sessions(BOB, 50);
    CHECK(bob_sessions.size() == 1);
    CHECK(bob_sessions[0].preview == "Bob's question");

    // Deleting again reports nothing removed, which is what lets the endpoint
    // answer 404 instead of pretending it deleted someone else's session.
    CHECK(store.delete_session(ALICE, shared) == 0);
    CHECK(store.delete_session(ALICE, "never-existed") == 0);

    // Bob's is still deletable afterwards — Alice's delete did not poison it.
    CHECK(store.delete_session(BOB, shared) == 1);
    CHECK(store.turn_count(BOB, shared) == 0);
    return 0;
}

int test_deleting_a_session_leaves_memories_alone() {
    // A conversation and the facts learned from it are different things. The
    // whole point of Funes is that forgetting the chat does not forget the
    // person, so deleting a session must not touch memories — not even the
    // auto-memories that conversation produced.
    MemoryStore store(temp_db("delsession_mem"), nullptr);

    store.append_turn(ALICE, "chat-1", "funes", "user", "my dog is called Zorbax");
    store.remember(ALICE, "funes", "The user's dog is called Zorbax", "auto");

    CHECK(store.delete_session(ALICE, "chat-1") == 1);
    CHECK(store.turn_count(ALICE, "chat-1") == 0);
    CHECK(store.count(ALICE) == 1);
    return 0;
}

int test_tool_results_are_isolated() {
    MemoryStore store(temp_db("results"), nullptr);
    const std::string shared = "s-collision";

    int64_t rid = store.store_result(ALICE, shared, "funes", "web_fetch", "alice private page");

    // Same session string, different user: must read as "not found".
    CHECK(!store.get_result(BOB, shared, rid).has_value());
    auto mine = store.get_result(ALICE, shared, rid);
    CHECK(mine.has_value());
    CHECK(*mine == "alice private page");

    // And a prune aimed at a shared session name must not take the other
    // user's results with it.
    store.store_result(BOB, shared, "funes", "web_fetch", "bob page");
    store.prune_results(BOB, shared);
    CHECK(store.get_result(ALICE, shared, rid).has_value());
    return 0;
}

int test_summaries_are_isolated() {
    MemoryStore store(temp_db("summary"), nullptr);
    const std::string shared = "s-collision";

    store.set_summary(ALICE, shared, "funes", "Alice has been discussing her salary");
    CHECK(store.get_summary(BOB, shared).empty());
    CHECK(store.get_summary(ALICE, shared) == "Alice has been discussing her salary");

    // Bob writing to the same session id must not overwrite Alice's summary.
    store.set_summary(BOB, shared, "funes", "Bob's summary");
    CHECK(store.get_summary(ALICE, shared) == "Alice has been discussing her salary");
    return 0;
}

int test_cron_jobs_are_owned() {
    MemoryStore store(temp_db("cron"), nullptr);

    MemoryStore::CronJob job;
    job.name = "alice nightly";
    job.kind = "agent";
    job.agent = "funes";
    job.task = "do the thing";
    job.schedule = "0 9 * * *";
    job.user_id = ALICE;
    job.next_run_at = 1000;
    int64_t id = store.create_cron_job(job);
    CHECK(id > 0);

    CHECK(store.list_cron_jobs(BOB).empty());
    CHECK(store.list_cron_jobs(ALICE).size() == 1);
    CHECK(store.list_cron_jobs(ALICE)[0].user_id == ALICE);

    CHECK(!store.get_cron_job(BOB, id).has_value());
    CHECK(store.get_cron_job(ALICE, id).has_value());
    // -1 is the runner's "any owner" path.
    CHECK(store.get_cron_job(-1, id).has_value());

    CHECK(!store.delete_cron_job(BOB, id));
    CHECK(store.list_cron_jobs(ALICE).size() == 1);
    CHECK(store.delete_cron_job(ALICE, id));

    // due_cron_jobs deliberately spans users — the poll loop serves everyone —
    // but each job has to carry the owner the runner will act as.
    job.user_id = BOB;
    store.create_cron_job(job);
    auto due = store.due_cron_jobs(2000);
    CHECK(due.size() == 1);
    CHECK(due[0].user_id == BOB);
    return 0;
}

int test_semantic_recall_is_isolated() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("semantic"), &emb);

    store.remember(ALICE, "funes", "the cat sat on the mat", "user");
    store.remember(BOB,   "funes", "the cat sat on the mat", "user");

    auto hits = store.recall(BOB, "funes", "the cat sat on the mat", 5);
    CHECK(!hits.empty());
    for (const auto& m : hits) CHECK(m.user_id == BOB);
    return 0;
}

int test_busy_neighbour_does_not_evict() {
    // The fidelity property, and the reason vec_memories has a PARTITION KEY.
    // Alice holds far more vectors than Bob and they are all near-identical to
    // Bob's query, so a KNN run across the whole table would fill its
    // candidate window with Alice's rows and leave Bob's own memory outside
    // it. Bob must still get his.
    FakeEmbedder emb;
    MemoryStore store(temp_db("crowding"), &emb);

    for (int i = 0; i < 200; ++i)
        store.remember(ALICE, "funes", "hiking trip notes number " + std::to_string(i), "auto");

    store.remember(BOB, "funes", "hiking trip notes for bob", "user");

    auto hits = store.recall(BOB, "funes", "hiking trip notes", 5);
    CHECK(!hits.empty());
    CHECK(hits[0].text == "hiking trip notes for bob");
    for (const auto& m : hits) CHECK(m.user_id == BOB);

    // Alice is unaffected by Bob's presence.
    CHECK(store.recall(ALICE, "funes", "hiking trip notes", 5).size() == 5);
    return 0;
}

int test_consolidation_does_not_merge_across_users() {
    // Two people stating the same fact are two facts. Merging them would
    // write one person's wording into the other's memory and delete a row
    // that was never theirs to delete.
    FakeEmbedder emb;
    MemoryStore store(temp_db("consolidate"), &emb);

    store.remember(ALICE, "funes", "the meeting is on tuesday", "auto");
    store.remember(ALICE, "funes", "the meeting is on tuesday!", "auto");
    store.remember(BOB,   "funes", "the meeting is on tuesday", "auto");

    std::vector<std::vector<std::string>> seen;
    auto merge = [&](const std::vector<std::string>& texts) {
        seen.push_back(texts);
        return std::string("the meeting is on tuesday");
    };

    MemoryStore::ConsolidationOptions opt;
    opt.prune_after_days = -1;      // isolate the merge step
    store.consolidate(merge, opt);

    // Every cluster handed to the model must have come from one user: three
    // near-identical texts exist, but only Alice's two may cluster.
    for (const auto& cluster : seen) CHECK(cluster.size() <= 2);

    // Bob's memory survives untouched.
    CHECK(store.count(BOB) == 1);
    auto bob = store.list(BOB, "", 10, 0);
    CHECK(bob.size() == 1);
    CHECK(bob[0].user_id == BOB);

    // The merged row must still be findable semantically. vec0 with a
    // partition key refuses INSERT OR REPLACE on an existing row, so the
    // re-vectorisation used to fail with a logged warning and leave the
    // merged memory keyword-only — the one row consolidation had just decided
    // was worth keeping.
    auto merged_hits = store.recall(ALICE, "funes", "the meeting is on tuesday", 5);
    CHECK(!merged_hits.empty());
    CHECK(merged_hits[0].user_id == ALICE);
    return 0;
}

int test_pre4_vec_table_is_rebuilt_before_first_recall() {
    // Regression: recall_semantic names v.user_id, which only exists on the
    // 4.0 partitioned vec table. The rebuild used to happen lazily inside
    // insert_vector, so an install that recalled before it wrote anything —
    // which is the normal order, since recall runs at the top of every turn —
    // hit "no such column: v.user_id" as a failed prepare and took the whole
    // agent run down with it. Found against a copy of a real 3.x database.
    const std::string path = temp_db("pre4vec");
    FakeEmbedder emb;

    {   // Build a store so the vec0 extension is registered process-wide,
        // then leave a memory behind for the recall to find.
        MemoryStore store(path, &emb);
        store.remember(ALICE, "funes", "the cat sat on the mat", "user");
    }

    // Rewrite the vector index into its pre-4.0 shape: no partition key, and
    // no marker saying it has one.
    {
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        const char* sql =
            "DROP TABLE IF EXISTS vec_memories;"
            "CREATE VIRTUAL TABLE vec_memories USING vec0("
            "  memory_id INTEGER PRIMARY KEY,"
            "  embedding float[26] distance_metric=cosine);"
            "DELETE FROM meta WHERE key='vec_partitioned';";
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        CHECK(rc == SQLITE_OK);
    }

    // Reopening must notice and rebuild. The recall is the assertion: before
    // the fix it threw rather than returning anything.
    MemoryStore store(path, &emb);
    auto hits = store.recall(ALICE, "funes", "the cat sat on the mat", 4);
    CHECK(!hits.empty());
    CHECK(hits[0].user_id == ALICE);
    return 0;
}

int test_workspaces_are_isolated() {
    // Phase 3: files, like memories, belong to one account. Two users writing
    // the same relative path must get two files, and neither must be able to
    // read the other's — the confinement in fs_guard::resolve is what stops
    // "../2/secret.txt" from being the way around it.
    fs::path root = fs::temp_directory_path() / "funes_iso_ws";
    fs::remove_all(root);

    ToolRegistry reg;
    register_file_tools(reg, root.string());

    ToolContext alice{"funes", "s1", "", "", ALICE};
    ToolContext bob  {"funes", "s1", "", "", BOB};

    CHECK(!reg.call("write_file", {{"path", "notes.txt"}, {"content", "alice salary"}},
                    alice).error);
    CHECK(!reg.call("write_file", {{"path", "notes.txt"}, {"content", "bob groceries"}},
                    bob).error);

    // Same relative path, two separate files.
    CHECK(fs::exists(root / "1" / "notes.txt"));
    CHECK(fs::exists(root / "2" / "notes.txt"));

    auto bob_read = reg.call("read_file", {{"path", "notes.txt"}}, bob);
    CHECK(!bob_read.error);
    CHECK(bob_read.text == "bob groceries");

    auto alice_read = reg.call("read_file", {{"path", "notes.txt"}}, alice);
    CHECK(alice_read.text == "alice salary");

    // Traversal out of one's own workspace into another's is refused.
    auto escape = reg.call("read_file", {{"path", "../1/notes.txt"}}, bob);
    CHECK(escape.error);
    auto abs_escape = reg.call("read_file",
                               {{"path", (root / "1" / "notes.txt").string()}}, bob);
    CHECK(abs_escape.error);

    // A relative agent workspace_dir override nests inside the caller, so two
    // users sharing one agent still get separate folders. This is what
    // whatsapp-autoresponder relies on.
    ToolContext alice_scoped{"wa", "s1", "whatsapp-uploads", "", ALICE};
    ToolContext bob_scoped  {"wa", "s1", "whatsapp-uploads", "", BOB};
    CHECK(!reg.call("write_file", {{"path", "doc.txt"}, {"content", "alice doc"}},
                    alice_scoped).error);
    CHECK(!reg.call("write_file", {{"path", "doc.txt"}, {"content", "bob doc"}},
                    bob_scoped).error);
    CHECK(fs::exists(root / "1" / "whatsapp-uploads" / "doc.txt"));
    CHECK(fs::exists(root / "2" / "whatsapp-uploads" / "doc.txt"));
    CHECK(reg.call("read_file", {{"path", "doc.txt"}}, bob_scoped).text == "bob doc");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_pre4_vec_table_is_rebuilt_before_first_recall();
    rc |= test_workspaces_are_isolated();
    rc |= test_memories_are_isolated();
    rc |= test_same_text_is_two_memories();
    rc |= test_forget_cannot_cross_users();
    rc |= test_turns_and_sessions_are_isolated();
    rc |= test_deleting_a_session_cannot_cross_users();
    rc |= test_deleting_a_session_leaves_memories_alone();
    rc |= test_tool_results_are_isolated();
    rc |= test_summaries_are_isolated();
    rc |= test_cron_jobs_are_owned();
    rc |= test_semantic_recall_is_isolated();
    rc |= test_busy_neighbour_does_not_evict();
    rc |= test_consolidation_does_not_merge_across_users();
    if (rc == 0) std::cout << "test_user_isolation: all passed\n";
    return rc;
}
