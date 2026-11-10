// =============================================================================
// src/core/auto_memory.h — what an exchange is stored as, when it is stored
// =============================================================================
// A Persist::Full run writes one `auto` memory per exchange (agent.cpp, step
// 5). The text of that memory is built here rather than inline so the recall
// evaluation (tests/memory_eval) stores its fixture exchanges exactly the way
// the agent does — a change to this function shows up in the eval's numbers.

#pragma once
#include <string>

namespace funes {

// The memory text for one exchange: `User said: "…" — I replied: "…"`.
std::string auto_memory_text(const std::string& user_message, const std::string& reply);

} // namespace funes
