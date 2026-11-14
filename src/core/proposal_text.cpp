// src/core/proposal_text.cpp — see proposal_text.h
#include "proposal_text.h"
#include "text_utils.h"
#include "tool_log.h"

namespace funes {

namespace {

std::string str(const nlohmann::json& args, const char* key) {
    if (!args.is_object() || !args.contains(key)) return "";
    const auto& v = args[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    return v.dump();
}

std::string quoted(std::string s, size_t n = 80) {
    if (s.size() > n) {
        truncate_utf8_safe(s, n);
        s += "…";
    }
    return "«" + s + "»";
}

} // namespace

std::string describe_call(const std::string& tool, const nlohmann::json& args, bool es) {
    auto say = [es](const std::string& en, const std::string& sp) { return es ? sp : en; };
    if (tool == "publish_issue") {
        const std::string pub = str(args, "publication").empty() ? "?" : str(args, "publication");
        const std::string date = str(args, "date").empty() ? say("today's", "de hoy") : str(args, "date");
        const bool sends = args.is_object() && args.value("send", true) && !args.value("dry_run", false);
        const std::string issue = es ? "el newsletter " + pub + " (" + date + ")"
                                     : "the " + pub + " newsletter (" + date + ")";
        return sends ? say("send " + issue + " to all its subscribers",
                           "enviar " + issue + " a todos sus suscriptores")
                     : say("build " + issue + " without sending it", "armar " + issue + " sin enviarlo");
    }
    if (tool == "run_script") {
        const std::string name = str(args, "name");
        const nlohmann::json a = args.is_object() ? args.value("arguments", nlohmann::json::object())
                                                  : nlohmann::json::object();
        const std::string with = a.empty() ? "" : " " + summarize_tool_args(a, 100);
        return say("run the script «" + name + "»", "correr el script «" + name + "»") + with;
    }
    if (tool == "run_job_now")
        return say("run scheduled job #" + str(args, "id") + " now",
                   "correr ahora el job programado #" + str(args, "id"));
    if (tool == "cancel_job")
        return say("delete scheduled job #" + str(args, "id") + " " + quoted(str(args, "name")),
                   "borrar el job programado #" + str(args, "id") + " " + quoted(str(args, "name")));
    if (tool == "schedule_job")
        return say("schedule a job " + quoted(str(args, "name")) + " (" + str(args, "schedule") + ")",
                   "programar un job " + quoted(str(args, "name")) + " (" + str(args, "schedule") + ")");
    if (tool == "notify_user")
        return say("send you a WhatsApp: ", "mandarte un WhatsApp: ") + quoted(str(args, "message"));
    if (tool == "execute_shell")
        return say("run in the shell: ", "ejecutar en la terminal: ") + quoted(str(args, "command"), 160);
    if (tool == "write_file")
        return say("write the file ", "escribir el archivo ") + str(args, "path");
    if (tool == "create_agent")
        return say("create the agent ", "crear el agente ") + quoted(str(args, "name"));
    if (tool == "send_message")
        return say("send a WhatsApp to " + str(args, "recipient") + ": ",
                   "mandar un WhatsApp a " + str(args, "recipient") + ": ") + quoted(str(args, "message"));
    return tool + " " + summarize_tool_args(args, 160);
}

std::string proposals_footer(const std::vector<Proposal>& made, bool es) {
    if (made.empty()) return "";
    std::string out = "\n\n—";
    for (const auto& p : made) {
        const std::string n = std::to_string(p.id);
        const std::string what = describe_call(p.tool, p.args, es);
        if (!p.stage.empty())
            out += es ? "\n📋 Propuesta #" + n + " (" + p.agent + "): aplicar los cambios de " + what +
                            ". «aprobar " + n + "» lo aplica, «rechazar " + n + "» lo descarta."
                      : "\n📋 Proposal #" + n + " (" + p.agent + "): apply the changes of " + what +
                            ". «approve " + n + "» applies it, «reject " + n + "» drops it.";
        else
            out += es ? "\n📋 Propuesta #" + n + " (" + p.agent + "): " + what + ". «aprobar " + n +
                            "» lo hace, «rechazar " + n + "» la descarta."
                      : "\n📋 Proposal #" + n + " (" + p.agent + "): " + what + ". «approve " + n +
                            "» does it, «reject " + n + "» drops it.";
    }
    return out;
}

} // namespace funes
