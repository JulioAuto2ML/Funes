// =============================================================================
// src/core/auto_memory.cpp — what an exchange is stored as
// =============================================================================

#include "auto_memory.h"

namespace funes {

std::string auto_memory_text(const std::string& user_message, const std::string& reply) {
    std::string r = reply.substr(0, 300);
    if (reply.size() > 300) r += "…";
    return "User said: \"" + user_message + "\" — I replied: \"" + r + "\"";
}

} // namespace funes
