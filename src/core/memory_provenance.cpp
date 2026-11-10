// =============================================================================
// src/core/memory_provenance.cpp — where a memory came from
// =============================================================================

#include "memory_provenance.h"
#include "text_utils.h"

namespace funes {

namespace {

// Built-ins whose results are the caller's own data or the server's own
// state. Everything else taints. read_file is here because the workspace is
// the person's own (they uploaded it); read_result is not, because a stored
// result is usually a web page or a search the run fetched earlier.
const std::set<std::string> kOwnData = {
    "remember", "recall",
    "read_file", "write_file", "list_files",
    "schedule_job", "list_jobs", "cancel_job",
    "list_scripts", "list_tools",
    "compress_context", "classify_decision",
    "create_agent", "create_tool",
};

// "https://user@www.Example.com:8443/path?q" → "www.example.com".
std::string host_of(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        host = host.substr(0, close == std::string::npos ? host.size() : close + 1);
    } else if (const size_t colon = host.find(':'); colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return host;
}

} // namespace

std::string RunProvenance::describe() const {
    std::string out;
    size_t shown = 0;
    for (const auto& s : sources) {
        if (shown == 3) {
            out += " +" + std::to_string(sources.size() - shown) + " more";
            break;
        }
        if (shown) out += ", ";
        out += s;
        ++shown;
    }
    return out;
}

std::string third_party_origin(const std::string& tool, const nlohmann::json& args) {
    if (kOwnData.count(tool)) return "";
    if (tool == "web_fetch") {
        const std::string host = host_of(json_string(args, "url"));
        return host.empty() ? "tool:web_fetch" : "url:" + host;
    }
    if (tool == "run_script") {
        const std::string name = json_string(args, "name");
        return name.empty() ? "tool:run_script" : "script:" + name;
    }
    if (tool == "delegate_to_agent") {
        const std::string name = json_string(args, "agent");
        return name.empty() ? "tool:delegate_to_agent" : "agent:" + name;
    }
    return "tool:" + tool;
}

std::string attributed(const std::string& text, const std::string& origin) {
    return "per " + (origin.empty() ? std::string("an outside source") : origin) + ": " + text;
}

} // namespace funes
