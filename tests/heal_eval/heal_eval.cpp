// =============================================================================
// tests/heal_eval/heal_eval.cpp — how often the doctor picks right
// =============================================================================
// Self-healing plan, H3/H4. Runs funes::doctor::diagnose — the same function
// the server calls — over fixture.json (this installation's incidents, each
// with the answer it should get) against a model, and prints the score. A
// doctor's choice is only ever proposed; raising that (H4) needs this number.
//
//   heal_eval --url http://127.0.0.1:8080 [--model default] [--repeat 3]
//   heal_eval --dry-run        # fixtures and policy load; no model (ctest)
//
// Run it on yoda against its own model — never a local one.
// =============================================================================

#include "doctor.h"
#include "heal.h"
#include "llm_client.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string url = "http://127.0.0.1:8080", model = "default";
    std::string fixture = FUNES_HEAL_FIXTURE, policy_path = FUNES_HEAL_POLICY_FILE;
    int repeat = 1;
    bool dry = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--url") url = next();
        else if (a == "--model") model = next();
        else if (a == "--repeat") repeat = std::max(1, std::atoi(next().c_str()));
        else if (a == "--fixture") fixture = next();
        else if (a == "--policy") policy_path = next();
        else if (a == "--dry-run") dry = true;
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }

    funes::heal::Policy policy;
    if (const std::string err = funes::heal::parse_policy(read_file(policy_path), policy); !err.empty()) {
        std::cerr << policy_path << ": " << err << "\n";
        return 1;
    }
    const auto cases = nlohmann::json::parse(read_file(fixture), nullptr, false);
    if (!cases.is_object() || !cases.contains("cases")) {
        std::cerr << fixture << ": not a fixture\n";
        return 1;
    }
    for (const auto& c : cases["cases"]) {
        const std::string expect = c.value("expect", "");
        bool known = expect == "explain";
        for (const auto& r : policy.runbooks) known = known || r.id == expect;
        if (!known) {
            std::cerr << c.value("name", "?") << ": expects '" << expect << "', not in the catalog\n";
            return 1;
        }
    }
    if (dry) {
        std::cout << "heal_eval: " << cases["cases"].size() << " cases, " << policy.runbooks.size()
                  << " runbooks — loaded\n";
        return 0;
    }

    LLMClient llm(url, "", model, "openai");
    llm.set_max_tokens(400);
    int right = 0, total = 0, refused = 0, unsafe = 0;
    for (const auto& c : cases["cases"]) {
        funes::doctor::Case k{c.value("key", ""), c.value("summary", ""), c.value("evidence", ""),
                              c.value("changes", "")};
        const std::string expect = c.value("expect", "");
        for (int r = 0; r < repeat; ++r) {
            std::string refusal;
            const auto v = funes::doctor::diagnose(
                [&llm](const std::vector<ChatMessage>& m) { return llm.complete(m).content; }, k,
                policy.runbooks, &refusal);
            ++total;
            const bool ok = v.choice == expect;
            right += ok;
            refused += !refusal.empty();
            // The costly mistake: a fix where a person was needed.
            unsafe += (!ok && expect == "explain");
            std::cout << (ok ? "  ok   " : "  MISS ") << c.value("name", "?") << ": " << v.choice
                      << (ok ? "" : " (expected " + expect + ")")
                      << (refusal.empty() ? "" : " [answer refused: " + refusal + "]") << "\n";
        }
    }
    std::cout << "\nheal_eval " << model << " @ " << url << ": " << right << "/" << total << " right ("
              << (total ? 100 * right / total : 0) << "%), " << unsafe
              << " fix(es) chosen where a person was needed, " << refused << " answer(s) refused\n";
    return 0;
}
