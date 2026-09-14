// =============================================================================
// src/core/lang_detect.cpp
// =============================================================================
// See lang_detect.h for why this is a word list rather than a model.
//
// The word lists are short on purpose: only words that are *frequent in one
// language and rare in the others*. "no" is common in both Spanish and
// English; "la" is Spanish, French and Italian. Words like those carry no
// signal and their only effect would be to make a close call closer, so they
// are left out even though they would be the first ones a frequency table
// suggested.

#include "lang_detect.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace funes {
namespace {

struct Lexicon {
    const char*              code;
    std::vector<std::string> words;
};

const std::vector<Lexicon>& lexicons() {
    static const std::vector<Lexicon> table = {
        {"en", {"the", "and", "is", "of", "to", "that", "with", "for", "was",
                "this", "have", "are", "not", "but", "from", "they", "what",
                "when", "which", "there", "about", "would", "should"}},
        {"es", {"el", "los", "las", "una", "que", "con", "para", "por", "pero",
                "como", "cuando", "porque", "también", "está", "están", "ser",
                "hacer", "tiene", "esto", "eso", "más", "muy", "sobre"}},
        {"pt", {"não", "uma", "com", "para", "mais", "como", "mas", "isso",
                "está", "são", "muito", "também", "quando", "porque", "fazer",
                "sobre", "até", "então", "ele", "ela", "dos", "das"}},
        {"fr", {"les", "des", "est", "une", "dans", "pour", "avec", "pas",
                "sur", "qui", "mais", "plus", "cette", "sont", "être", "avoir",
                "faire", "aussi", "alors", "chez"}},
        {"it", {"che", "non", "per", "con", "sono", "come", "anche", "questo",
                "questa", "quando", "perché", "molto", "essere", "fare", "dei",
                "delle", "nella", "sulla"}},
        {"de", {"der", "die", "das", "und", "ist", "nicht", "mit", "auch",
                "eine", "einen", "für", "auf", "sich", "wird", "werden",
                "haben", "sein", "aber", "oder", "wenn"}},
    };
    return table;
}

// Lowercased, split on anything that is not a letter. Multi-byte UTF-8 bytes
// (>= 0x80) are kept verbatim and pass through untouched, which is what lets
// "también" and "não" match as written — they are the highest-signal words in
// their lists precisely because the accent makes them unambiguous.
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalpha(c) || c >= 0x80) {
            current += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        } else if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

} // namespace

std::string detect_language(const std::string& text) {
    const std::vector<std::string> words = tokenize(text);
    // Below this there is not enough text for a word list to mean anything:
    // "Julio's birthday is in May" and "El cumpleaños es en mayo" are both
    // mostly content words, and a detector that answers anyway is guessing.
    if (words.size() < 4) return {};

    std::map<std::string, int> hits;
    for (const auto& w : words)
        for (const auto& lex : lexicons())
            if (std::find(lex.words.begin(), lex.words.end(), w) != lex.words.end())
                ++hits[lex.code];

    if (hits.empty()) return {};

    std::string best;
    int best_score = 0, runner_up = 0;
    for (const auto& [code, score] : hits) {
        if (score > best_score) { runner_up = best_score; best = code; best_score = score; }
        else if (score > runner_up) { runner_up = score; }
    }

    // Two thresholds, both about refusing to answer rather than about being
    // right. A single stopword in a long sentence is noise, and a language
    // that barely beats the next one is a coin flip — an unlabelled memory is
    // more honest than either, and nothing downstream depends on the label.
    if (best_score < 2) return {};
    if (best_score == runner_up) return {};
    return best;
}

} // namespace funes
