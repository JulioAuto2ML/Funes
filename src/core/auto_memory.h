// =============================================================================
// src/core/auto_memory.h — what an exchange is stored as, when it is stored
// =============================================================================
// A Persist::Full run writes one `auto` memory per exchange (agent.cpp, step
// 5). The text of that memory is built here rather than inline so the recall
// evaluation (tests/memory_eval) stores its fixture exchanges exactly the way
// the agent does — a change to this function shows up in the eval's numbers.
//
// Bounded since 7.1 (docs/memory-plan.md, phase 2). The user half used to be
// stored whole, so an attached document became the memory: 14.5k chars on
// yoda, too long to embed and a quarter of a small model's context if
// recalled. Attachments are now kept as their name — the file itself is in
// the workspace — and both halves are capped at kHalfCap.

#pragma once
#include <cstddef>
#include <string>

namespace funes {

constexpr size_t kHalfCap = 300;   // bytes, per half of an exchange

// Shortens text to at most max_bytes (plus "…"), at the last sentence end in
// the second half of the window if there is one, else the last space, else
// the last whole UTF-8 character. Text that already fits is returned as is.
std::string shorten_on_sentence(const std::string& text, size_t max_bytes);

// The UI's attachment blocks (ui/app.js, buildFullText) reduced to
// "[attached: <name>]", and whitespace runs collapsed to one space.
std::string without_attachments(const std::string& user_message);

// The memory text for one exchange: `User said: "…" — I replied: "…"`.
std::string auto_memory_text(const std::string& user_message, const std::string& reply);

} // namespace funes
