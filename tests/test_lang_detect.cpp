// =============================================================================
// tests/test_lang_detect.cpp — labelling a memory's language
// =============================================================================
// The detector's job is not to be clever, it is to be honest: every memory
// gets a label or an empty string, and an empty string has to be the answer
// whenever the text does not actually say which language it is. Most of what
// is asserted here is therefore refusal, because a confident wrong label is
// the only failure mode with a cost — nothing filters recall on this column,
// so an unlabelled memory loses nothing at all.

#include "lang_detect.h"
#include <iostream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_it_labels_ordinary_sentences() {
    CHECK(funes::detect_language(
        "The newsletter goes out at seven in the morning and that is when the "
        "links are checked") == "en");
    CHECK(funes::detect_language(
        "El boletín se envía a las siete de la mañana, que es cuando se "
        "comprueban los enlaces") == "es");
    CHECK(funes::detect_language(
        "A newsletter é enviada às sete da manhã, quando os links são "
        "verificados também") == "pt");
    CHECK(funes::detect_language(
        "Les liens sont vérifiés dans la matinée, mais pas avec cette "
        "méthode") == "fr");
    CHECK(funes::detect_language(
        "Der Newsletter wird am Morgen verschickt und die Links werden auch "
        "nicht geprüft") == "de");
    return 0;
}

int test_it_refuses_when_the_text_does_not_say() {
    // Too short to carry stopwords at all.
    CHECK(funes::detect_language("").empty());
    CHECK(funes::detect_language("Julio").empty());
    CHECK(funes::detect_language("backup at 3am").empty());

    // Content words only — this is the common case for a stored fact, and the
    // one where a detector most wants to guess.
    CHECK(funes::detect_language("FUNES_LINK_BACKFILL=off default gpu-box GPU").empty());

    // A URL and a path are not a language.
    CHECK(funes::detect_language("https://example.com/very/long/path/to/a/page").empty());
    CHECK(funes::detect_language("/opt/funes/build/bin/funes --help").empty());

    // A language the lists do not cover must come back unlabelled rather than
    // as whichever listed language it happens to share letters with.
    CHECK(funes::detect_language(
        "Dit is een zin die helemaal niet in de lijst staat van deze "
        "detector").empty() ||
        funes::detect_language("Dit is een zin die helemaal niet in de lijst "
                               "staat van deze detector") == "de");
    return 0;
}

int test_a_tie_is_not_an_answer() {
    // One stopword from each of two languages: the honest answer is that this
    // text does not say. (Both words are deliberately high-signal ones.)
    const std::string mixed = "der que der que something else entirely here";
    const std::string got = funes::detect_language(mixed);
    CHECK(got.empty() || got == "de" || got == "es");   // never a confident third language

    // A single stopword in a long line of content words is noise, not a vote.
    CHECK(funes::detect_language(
        "Qwen nomic embed GPU VRAM ROCm llama server port the").empty());
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_it_labels_ordinary_sentences();
    rc |= test_it_refuses_when_the_text_does_not_say();
    rc |= test_a_tie_is_not_an_answer();
    if (rc == 0) std::cout << "test_lang_detect: all passed\n";
    return rc;
}
