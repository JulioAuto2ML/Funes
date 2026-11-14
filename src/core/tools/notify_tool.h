// =============================================================================
// src/core/tools/notify_tool.h — notify_user
// =============================================================================
// Sends a WhatsApp message to the person a run belongs to, from Funes's own
// number (the whatsapp-bridge-funes bridge, WHATSAPP_BRIDGE_URL). It is what a
// scheduled reminder needs and what nothing else gave it.
//
// Before 7.17 a reminder had two ways to go, both wrong. One ran
// whatsapp-assistant, whose bridge is the person's *own* number — that agent
// exists to act as them in their chats — so "remember your Relvar" arrived
// from the person to the person. The other ran funes, which can send nothing:
// a cron job's answer is only stored in cron_jobs.last_output, so the
// 23/12 doctor's appointment reminder would have run "ok" and reached nobody.
//
// The recipient is never an argument. It is the jid mapped to the run's
// account (`funes jid-map`), so the tool is "tell my person", not "send a
// WhatsApp" — a model cannot point it at a third party, and a member without a
// mapping gets an error, never the admin's number. It is still `external` in
// effects.h: a tainted interactive run proposes it, a scheduled job (whose
// scheduling was the approval) sends at once.
// =============================================================================

#pragma once
#include "../tools.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Long enough for any reminder or alert; a model pasting a whole report into a
// WhatsApp message is the case this refuses.
constexpr size_t kMaxNotifyChars = 2000;

struct NotifyChannel {
    // The account's WhatsApp jid; "" when it has none.
    std::function<std::string(int64_t user_id)> jid_for;
    // Delivers `text` to `jid`. "" on success, otherwise why not.
    std::function<std::string(const std::string& jid, const std::string& text)> send;
};

void register_notify_tool(ToolRegistry& reg, NotifyChannel channel);

// The real `send`: POST {recipient, message} to `<bridge_url>/api/send`, the
// whatsapp-mcp bridge's API (the autoresponder and publishing/linkedin_token.py
// use the same call). `bridge_url` is configuration, never model input.
std::string send_via_whatsapp_bridge(const std::string& bridge_url,
                                     const std::string& jid, const std::string& text);
