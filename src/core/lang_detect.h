// =============================================================================
// src/core/lang_detect.h — which language a remembered fact is in
// =============================================================================
// Recorded on every memory at remember() time (5.0), so the store knows what
// it is holding: a pool that is 60% English and 40% Spanish is one pool to a
// multilingual embedding model, but it is two to a keyword search, and "show
// me what I know in Spanish" is a question the UI can only answer if the
// column exists.
//
// Stopword frequency, not a model and not a library. Three reasons that is the
// right size here:
//
//   * It runs inside remember(), on the write path, for every memory. A model
//     call there would put a network round-trip in front of storing a fact.
//   * The languages that actually occur in one household are a handful, and
//     stopwords separate them almost perfectly at sentence length.
//   * Being wrong is cheap. The column is descriptive — nothing filters recall
//     on it — so a misdetection costs a wrong label, not a lost memory.
//
// Which is also why `detect_language` returns "" rather than guessing when the
// evidence is thin. An unknown language is a fact about the text; a confident
// wrong answer is a fact about the detector.

#pragma once
#include <string>

namespace funes {

// "en", "es", "pt", "fr", "it", "de", or "" when nothing scores clearly enough
// — a URL, a code snippet, a three-word note, or a language not listed here.
// Never throws.
std::string detect_language(const std::string& text);

} // namespace funes
