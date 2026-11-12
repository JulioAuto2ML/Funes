// src/core/effects.cpp — see effects.h
#include "effects.h"
#include <cctype>
#include <map>

namespace funes {

std::optional<Effect> parse_effect(const std::string& s) {
    if (s == "none")      return Effect::None;
    if (s == "workspace") return Effect::Workspace;
    if (s == "host")      return Effect::Host;
    if (s == "external")  return Effect::External;
    return std::nullopt;
}

const char* effect_name(Effect e) {
    switch (e) {
        case Effect::None:      return "none";
        case Effect::Workspace: return "workspace";
        case Effect::Host:      return "host";
        case Effect::External:  return "external";
    }
    return "external";
}

Effect builtin_effect(const std::string& tool) {
    static const std::map<std::string, Effect> kEffects = {
        {"write_file",       Effect::Workspace},
        {"write_structured", Effect::Workspace},
        {"execute_shell",    Effect::Host},
        {"create_agent",     Effect::Host},
        {"create_tool",      Effect::Host},
        {"publish_issue",    Effect::External},   // the newsletter, to its subscribers
        // A job runs unattended and ungated later; scheduling one is the
        // approval, so a tainted run must not be able to give it.
        {"schedule_job",     Effect::External},
        {"run_job_now",      Effect::External},
    };
    auto it = kEffects.find(tool);
    return it == kEffects.end() ? Effect::None : it->second;
}

std::optional<ApprovalCommand> parse_approval(const std::string& message) {
    // Lower-case ASCII, fold the one accent the Spanish verbs can carry, and
    // drop surrounding blanks and a trailing full stop or "!".
    std::string s;
    for (size_t i = 0; i < message.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(message[i]);
        if (c == 0xC3 && i + 1 < message.size()) {
            const unsigned char d = static_cast<unsigned char>(message[i + 1]);
            if (d == 0xA1 || d == 0x81) { s += 'a'; ++i; continue; }   // á Á
        }
        s += static_cast<char>(std::tolower(c));
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '.' || s.back() == '!'))
        s.pop_back();
    size_t pos = 0;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;

    static const std::pair<const char*, bool> kVerbs[] = {
        {"aprobar", true}, {"apruebo", true}, {"approve", true},
        {"rechazar", false}, {"rechazo", false}, {"reject", false},
    };
    ApprovalCommand cmd;
    bool matched = false;
    for (const auto& [verb, approve] : kVerbs) {
        const std::string v = verb;
        if (s.compare(pos, v.size(), v) == 0) {
            cmd.approve = approve;
            pos += v.size();
            matched = true;
            break;
        }
    }
    if (!matched || pos >= s.size() || !std::isspace(static_cast<unsigned char>(s[pos]))) return std::nullopt;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    if (s.compare(pos, 1, "#") == 0) ++pos;
    else if (s.compare(pos, 2, "p-") == 0) pos += 2;

    const size_t digits = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos == digits || pos != s.size() || pos - digits > 15) return std::nullopt;
    cmd.id = std::stoll(s.substr(digits));
    if (cmd.id <= 0) return std::nullopt;
    return cmd;
}

} // namespace funes
