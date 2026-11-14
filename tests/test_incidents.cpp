// =============================================================================
// tests/test_incidents.cpp — IncidentStore (self-healing H0)
// =============================================================================
// What matters: a check failing every five minutes is one incident and one
// notice, not 288; a flaky check is not an incident; a notice that could not
// go out is still pending later; and a check nobody reports any more does not
// stay open forever.

#include "incidents.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace funes::incidents;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_test_incidents_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

CheckResult fail(const std::string& key, const std::string& summary = "down") {
    return {key, false, "warning", summary, "connection refused"};
}
CheckResult pass(const std::string& key) { return {key, true, "", "up", ""}; }

} // namespace

int test_one_incident_while_it_keeps_failing() {
    IncidentStore s(temp_db("one"));
    CHECK(s.observe(fail("p:llm"), 1, 100) == Transition::Opened);
    CHECK(s.observe(fail("p:llm", "still down"), 1, 400) == Transition::None);
    CHECK(s.observe(fail("p:llm"), 1, 700) == Transition::None);
    auto open = s.open_incidents();
    CHECK(open.size() == 1);
    CHECK(open[0].occurrences == 3);
    CHECK(open[0].opened_at == 100);
    CHECK(open[0].last_seen_at == 700);
    CHECK(s.pending_notices().size() == 1);

    CHECK(s.observe(pass("p:llm"), 1, 1000) == Transition::Resolved);
    CHECK(s.open_incidents().empty());
    CHECK(s.get(open[0].id)->resolved_at == 1000);
    // Passing again is nothing; failing again is a new incident.
    CHECK(s.observe(pass("p:llm"), 1, 1300) == Transition::None);
    CHECK(s.observe(fail("p:llm"), 1, 1600) == Transition::Opened);
    CHECK(s.recent(0).size() == 2);
    return 0;
}

// fail_after: two failures in a row, not two failures ever.
int test_a_flaky_check_is_not_an_incident() {
    IncidentStore s(temp_db("flaky"));
    CHECK(s.observe(fail("p:internet"), 2, 100) == Transition::None);
    CHECK(s.observe(pass("p:internet"), 2, 200) == Transition::None);
    CHECK(s.observe(fail("p:internet"), 2, 300) == Transition::None);
    CHECK(s.open_incidents().empty());
    CHECK(s.observe(fail("p:internet"), 2, 400) == Transition::Opened);
    CHECK(s.open_incidents()[0].opened_at == 400);
    return 0;
}

int test_notices_stay_pending_until_marked() {
    IncidentStore s(temp_db("notice"));
    s.observe(fail("p:a"), 1, 100);
    s.observe(fail("p:b"), 1, 100);
    auto pending = s.pending_notices();
    CHECK(pending.size() == 2);
    s.mark_noticed(pending[0].id, false);
    CHECK(s.pending_notices().size() == 1);

    // Resolving a told incident makes its "resolved" pending.
    s.observe(pass(pending[0].key), 1, 200);
    pending = s.pending_notices();
    CHECK(pending.size() == 2);
    for (const auto& i : pending) s.mark_noticed(i.id, i.resolved_at > 0);
    CHECK(s.pending_notices().empty());   // p:b is still open, but already told
    return 0;
}

// Opened and closed during quiet hours: one notice, saying both.
int test_resolved_before_told_is_one_notice() {
    IncidentStore s(temp_db("both"));
    s.observe(fail("p:x"), 1, 100);
    s.observe(pass("p:x"), 1, 200);
    auto pending = s.pending_notices();
    CHECK(pending.size() == 1);
    CHECK(!pending[0].noticed_open && pending[0].resolved_at == 200);
    s.mark_noticed(pending[0].id, true);
    CHECK(s.pending_notices().empty());
    return 0;
}

int test_unreported_checks_are_resolved() {
    IncidentStore s(temp_db("unreported"));
    s.observe(fail("jobs:1"), 1, 100);
    s.observe(fail("jobs:7"), 1, 100);
    s.observe(fail("probe_services:llm"), 1, 100);
    // The jobs probe ran and only reported job 7: job 1 was deleted.
    CHECK(s.resolve_unreported("jobs:", {"jobs:7"}, 200) == 1);
    auto open = s.open_incidents();
    CHECK(open.size() == 2);
    for (const auto& i : open) CHECK(i.key != "jobs:1");
    return 0;
}

int test_evidence_is_capped() {
    IncidentStore s(temp_db("cap"));
    CheckResult r = fail("p:log");
    r.evidence = std::string(5000, 'x');
    s.observe(r, 1, 100);
    CHECK(s.open_incidents()[0].evidence.size() <= kMaxEvidenceBytes + 3);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_one_incident_while_it_keeps_failing();
    rc |= test_a_flaky_check_is_not_an_incident();
    rc |= test_notices_stay_pending_until_marked();
    rc |= test_resolved_before_told_is_one_notice();
    rc |= test_unreported_checks_are_resolved();
    rc |= test_evidence_is_capped();
    if (rc == 0) std::cout << "test_incidents: all passed\n";
    return rc;
}
