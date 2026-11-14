// src/core/tools/notify_tool.cpp — see notify_tool.h
#include "notify_tool.h"
#include "httplib.h"
#include <iostream>

namespace {

size_t utf8_chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

bool blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

// The log names the account and the jid's last digits, never the text: a
// reminder can be about medication or an appointment.
std::string jid_tail(const std::string& jid) {
    const size_t at = jid.find('@');
    const std::string id = jid.substr(0, at);
    return "…" + (id.size() > 4 ? id.substr(id.size() - 4) : id);
}

ToolResult notify_handler(const NotifyChannel& ch, const json& args, const ToolContext& ctx) {
    if (!args.contains("message") || !args["message"].is_string())
        return {"Missing or invalid 'message' argument (the text to send)", true};
    const std::string text = args["message"].get<std::string>();
    if (blank(text)) return {"'message' is empty; there is nothing to send.", true};
    const size_t chars = utf8_chars(text);
    if (chars > kMaxNotifyChars)
        return {"'message' is " + std::to_string(chars) + " characters; the limit is " +
                std::to_string(kMaxNotifyChars) + ". Send a short notice, not a report.", true};

    const std::string jid = ch.jid_for ? ch.jid_for(ctx.user_id) : "";
    if (jid.empty()) {
        std::cerr << "[notify_user] user " << ctx.user_id << ": no WhatsApp number mapped\n";
        return {"This account has no WhatsApp number mapped, so there is nobody to send to. "
                "An admin maps one with `funes jid-map <jid> <user>`. Say so; do not try "
                "another way of sending.", true};
    }
    const std::string err = ch.send ? ch.send(jid, text) : "no WhatsApp bridge configured";
    std::cerr << "[notify_user] user " << ctx.user_id << " → " << jid_tail(jid) << ", "
              << chars << " chars: " << (err.empty() ? "sent" : "FAILED (" + err + ")") << "\n";
    if (!err.empty())
        return {"The message was NOT sent: " + err + ". Report that it failed.", true};
    return {"Sent to the person's WhatsApp from Funes's number."};
}

} // namespace

std::string send_via_whatsapp_bridge(const std::string& bridge_url,
                                     const std::string& jid, const std::string& text) {
    if (bridge_url.empty()) return "WHATSAPP_BRIDGE_URL is not set";
    httplib::Client cli(bridge_url);
    if (!cli.is_valid()) return "WHATSAPP_BRIDGE_URL is not a valid URL";
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_follow_location(false);
    const json body = {{"recipient", jid}, {"message", text}};
    auto res = cli.Post("/api/send", body.dump(), "application/json");
    if (!res) return "cannot reach the WhatsApp bridge (" + httplib::to_string(res.error()) + ")";
    if (res->status != 200)
        return "the WhatsApp bridge answered HTTP " + std::to_string(res->status);
    // {"success": false, "message": "..."} is a refusal with a 200.
    const json reply = json::parse(res->body, nullptr, false);
    if (reply.is_object() && reply.contains("success") && reply["success"].is_boolean() &&
        !reply["success"].get<bool>()) {
        std::string why = reply.contains("message") && reply["message"].is_string()
                              ? reply["message"].get<std::string>() : "no reason given";
        if (why.size() > 200) why.resize(200);
        return "the WhatsApp bridge refused it: " + why;
    }
    return "";
}

void register_notify_tool(ToolRegistry& reg, NotifyChannel channel) {
    reg.add({
        "notify_user",
        "Send a short WhatsApp message to the person you are working for, from Funes's own "
        "number. Use it for reminders and alerts they asked for, above all in scheduled "
        "jobs, whose answer otherwise reaches nobody. It always goes to that person and "
        "only to them; there is no recipient to choose. Not for messaging anyone else.",
        {
            {"type", "object"},
            {"properties", {
                {"message", {{"type", "string"},
                             {"description", "The text to send, exactly as the person should read it"}}}
            }},
            {"required", json::array({"message"})}
        },
        [channel](const json& args, const ToolContext& ctx) {
            return notify_handler(channel, args, ctx);
        }
    });
}
