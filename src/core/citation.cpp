// =============================================================================
// src/core/citation.cpp — was a recalled memory used?
// =============================================================================

#include "citation.h"
#include <algorithm>

namespace funes {

namespace {

// Words of four or more letters that say nothing about which memory a reply
// drew on. "user"/"usuario" are here because memories are written in the
// third person: every one of them names the user.
const std::set<std::string> kCommon = {
    "about", "after", "also", "been", "before", "being", "could", "does", "each",
    "every", "from", "have", "here", "into", "just", "like", "more", "most",
    "only", "other", "over", "said", "same", "should", "some", "such", "than",
    "that", "their", "them", "then", "there", "these", "they", "this", "those",
    "very", "want", "were", "what", "when", "where", "which", "while", "will",
    "with", "would", "your", "yours", "user", "user's", "replied",
    "ahora", "antes", "como", "cómo", "cuando", "desde", "donde", "entre",
    "esta", "está", "este", "esto", "hace", "hasta", "mucho", "muy", "nada",
    "otra", "otro", "para", "pero", "porque", "puede", "sobre", "también",
    "tiene", "todo", "todos", "tengo", "usuario", "usuaria",
};

} // namespace

std::set<std::string> content_terms(const std::string& text) {
    std::set<std::string> out;
    std::string word;
    auto flush = [&] {
        if (word.size() >= 4 && !kCommon.count(word)) out.insert(word);
        word.clear();
    };
    for (unsigned char c : text) {
        if (c >= 0x80 || std::isalnum(c) || c == '\'') {
            word += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        } else {
            flush();
        }
    }
    flush();
    return out;
}

bool is_cited(const std::string& memory_text, const std::string& reply) {
    const auto mem = content_terms(memory_text);
    if (mem.empty()) return false;
    const auto rep = content_terms(reply);
    size_t overlap = 0;
    for (const auto& t : mem) overlap += rep.count(t);
    // Two terms, and at least half of the memory's: two shared topic words
    // ("spanish", "newsletter") are not a citation of every memory about the
    // newsletter.
    return overlap >= std::min<size_t>(2, mem.size()) && overlap * 2 >= mem.size();
}

} // namespace funes
