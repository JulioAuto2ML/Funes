// src/core/heal.cpp — see heal.h
#include "heal.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace funes::heal {

namespace {

constexpr int64_t kDay = 86400;
// `act` per incident key in a day before it proposes instead.
constexpr size_t kActsPerKeyPerDay = 2;

nlohmann::json yaml_to_json(const YAML::Node& n) {
    if (n.IsMap()) {
        nlohmann::json o = nlohmann::json::object();
        for (const auto& kv : n) o[kv.first.as<std::string>()] = yaml_to_json(kv.second);
        return o;
    }
    if (n.IsSequence()) {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& v : n) a.push_back(yaml_to_json(v));
        return a;
    }
    if (n.IsNull()) return nullptr;
    const std::string s = n.as<std::string>();
    if (n.Tag() != "!" ) {   // unquoted: numbers and booleans are what they look like
        if (s == "true") return true;
        if (s == "false") return false;
        if (!s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::stoll(s);
    }
    return s;
}

// "${match}" — what a `when: "jobs:*"` matched ("7") — as a number when it is one.
nlohmann::json substitute(const nlohmann::json& v, const std::string& matched) {
    if (v.is_object()) {
        nlohmann::json o = nlohmann::json::object();
        for (auto it = v.begin(); it != v.end(); ++it) o[it.key()] = substitute(it.value(), matched);
        return o;
    }
    if (v.is_array()) {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& x : v) a.push_back(substitute(x, matched));
        return a;
    }
    if (v.is_string() && v.get<std::string>() == "${match}") {
        if (!matched.empty() && matched.size() < 18 &&
            std::all_of(matched.begin(), matched.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::stoll(matched);
        return matched;
    }
    return v;
}

bool matches(const std::string& when, const std::string& key, std::string* rest = nullptr) {
    if (!when.empty() && when.back() == '*') {
        const std::string prefix = when.substr(0, when.size() - 1);
        if (key.compare(0, prefix.size(), prefix) != 0) return false;
        // "<probe>:(run)" is the probe itself failing (probes.h), not one of
        // the things it checks: "jobs:*" must not propose retrying job "(run)".
        if (key.size() >= 6 && key.compare(key.size() - 6, 6, ":(run)") == 0) return false;
        if (rest) *rest = key.substr(prefix.size());
        return true;
    }
    return when == key;
}

void keep_last_day(std::vector<int64_t>& v, int64_t now) {
    v.erase(std::remove_if(v.begin(), v.end(), [now](int64_t t) { return now - t >= kDay; }), v.end());
}

} // namespace

std::string parse_policy(const std::string& yaml_text, Policy& out) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        return std::string("not YAML: ") + e.what();
    }
    Policy p;
    if (!root || root.IsNull()) { out = p; return ""; }
    if (!root.IsMap() || !root["rules"] || !root["rules"].IsSequence())
        return "the policy is a map with a `rules:` list";
    int n = 0;
    for (const auto& node : root["rules"]) {
        ++n;
        const std::string at = "rule " + std::to_string(n);
        if (!node.IsMap() || !node["when"]) return at + ": needs `when:` (an incident key, or a prefix ending in *)";
        Rule r;
        auto str = [&node](const char* k) { return node[k] ? node[k].as<std::string>() : std::string(); };
        r.when = str("when");
        const std::string level = node["level"] ? str("level") : "explain";
        if (level == "observe") r.level = Level::Observe;
        else if (level == "explain") r.level = Level::Explain;
        else if (level == "propose") r.level = Level::Propose;
        else if (level == "act") r.level = Level::Act;
        else return at + " (" + r.when + "): level is observe, explain, propose or act";
        r.action = str("action");
        r.agent = str("agent");
        r.tool = str("tool");
        r.undo = str("undo");
        r.explain = str("explain");
        if (node["args"]) {
            if (!node["args"].IsMap()) return at + " (" + r.when + "): args is a map";
            r.args = yaml_to_json(node["args"]);
        }
        if (node["max_per_day"]) {
            try { r.max_per_day = node["max_per_day"].as<int>(); }
            catch (const YAML::Exception&) { return at + ": max_per_day is a whole number"; }
        }
        if (r.level == Level::Explain && r.explain.empty())
            return at + " (" + r.when + "): explain needs `explain:` — what a person should do";
        if (r.level >= Level::Propose) {
            if (r.agent.empty() || r.tool.empty() || r.action.empty())
                return at + " (" + r.when + "): propose/act needs agent, tool and action";
            if (r.undo != "none")
                return at + " (" + r.when + "): propose/act needs `undo: none` — the only kind H2 has "
                       "(a restart or a retry leaves nothing to put back); a runbook that writes "
                       "needs the guardian's snapshot, which is not wired yet";
        }
        p.rules.push_back(std::move(r));
    }
    out = std::move(p);
    return "";
}

const Rule* match(const Policy& policy, const std::string& key) {
    for (const auto& r : policy.rules)
        if (matches(r.when, key)) return &r;
    return nullptr;
}

Healer::Healer(Policy policy, Options opt) : policy_(std::move(policy)), opt_(std::move(opt)) {}

std::string Healer::on_open(const incidents::Incident& i) {
    const Rule* found = nullptr;
    std::string matched;
    for (const auto& r : policy_.rules)
        if (matches(r.when, i.key, &matched)) { found = &r; break; }
    if (!found || found->level == Level::Observe) return "";

    Rule rule = *found;
    rule.args = substitute(rule.args, matched);
    const bool es = opt_.locale.rfind("es", 0) == 0;
    auto say = [es](const std::string& en, const std::string& sp) { return es ? sp : en; };
    const std::string hint = rule.explain.empty() ? "" : say("\n\nWhat to do: ", "\n\nQué hacer: ") + rule.explain;
    if (rule.level == Level::Explain) return hint;

    const int64_t now = opt_.now ? opt_.now() : 0;
    Level level = rule.level;
    std::string held;
    if (level == Level::Act) {
        auto& by_key = acted_by_key_[i.key];
        auto& by_rule = acted_by_rule_[rule.when];
        keep_last_day(by_key, now);
        keep_last_day(by_rule, now);
        if (by_key.size() >= kActsPerKeyPerDay) {
            level = Level::Propose;
            held = say("I already did this " + std::to_string(by_key.size()) + " times in 24 h and it did not hold, so this time I ask. ",
                       "Ya lo hice " + std::to_string(by_key.size()) + " veces en 24 h y no se sostuvo, así que esta vez pregunto. ");
        } else if (static_cast<int>(by_rule.size()) >= rule.max_per_day) {
            level = Level::Propose;
            held = say("This rule already acted " + std::to_string(by_rule.size()) + " times today. ",
                       "Esta regla ya actuó " + std::to_string(by_rule.size()) + " veces hoy. ");
        }
    }

    if (level == Level::Act) {
        acted_by_key_[i.key].push_back(now);
        acted_by_rule_[rule.when].push_back(now);
        const auto [ok, out] = opt_.act ? opt_.act(rule, i) : std::make_pair(false, std::string("no runner"));
        std::string detail = out.substr(0, 300);
        std::cerr << "[heal] " << i.key << ": acted (" << rule.action << ") → " << (ok ? "ok" : "FAILED") << "\n";
        return say("\n\n🔧 Already done: ", "\n\n🔧 Ya hice: ") + rule.action +
               (ok ? say(". I will tell you when the check passes again.", ". Te aviso cuando el chequeo vuelva a pasar.")
                   : say(" — it failed: ", " — falló: ") + detail) + hint;
    }

    const int64_t id = opt_.propose ? opt_.propose(rule, i) : 0;
    std::cerr << "[heal] " << i.key << ": proposed (" << rule.action << ") → "
              << (id ? "#" + std::to_string(id) : std::string("FAILED")) << "\n";
    if (!id) return say("\n\nI wanted to propose: ", "\n\nQuise proponer: ") + rule.action +
                    say(", but could not store the proposal.", ", pero no pude guardar la propuesta.") + hint;
    return "\n\n" + held + say("I propose: ", "Propongo: ") + rule.action + ". " +
           say("To do it: «approve " + std::to_string(id) + "».", "Para hacerlo: «aprobar " + std::to_string(id) + "».") + hint;
}

} // namespace funes::heal
