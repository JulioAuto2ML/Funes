// =============================================================================
// src/core/citation.h — was a recalled memory used? (memory plan, phase 7)
// =============================================================================
// Pruning used to drop `auto` memories that were never *recalled*. On yoda
// 175 of 209 had been recalled at least once — mostly by questions like the
// ones they came from — so the clutter protected itself. Recalled is not
// used: four memories are put in front of the model on every turn whether or
// not the answer needs them.
//
// A memory counts as cited by a reply when the reply repeats its distinctive
// words. Deterministic and cheap: lowercased terms of four or more bytes,
// common words excluded; the reply must repeat two of them (all of a one-term
// memory) and at least half — two shared topic words are not a citation of
// every memory on that topic. It undercounts — a reply that paraphrases cites
// nothing — which is the safe direction for something that feeds eviction: an
// uncited memory is only archived after a long window, and archiving is
// reversible.

#pragma once
#include <set>
#include <string>

namespace funes {

// The distinctive terms of `text`: lowercased (ASCII; other UTF-8 bytes kept
// as they are), at least four bytes, not a common English/Spanish word.
std::set<std::string> content_terms(const std::string& text);

bool is_cited(const std::string& memory_text, const std::string& reply);

} // namespace funes
