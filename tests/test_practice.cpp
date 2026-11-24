// =============================================================================
// tests/test_practice.cpp — the practice exercise library (practice/)
// =============================================================================
// Note names, the pattern mini-language, the root walk that fits a pattern to
// a singer's range, exercise validation, and — given the shipped practice/
// directory — that every exercise in it loads and fits every default voice.
// A model never writes notes; these files are the only place they come from,
// so a typo in one is a wrong note sung by a person.

#include "practice.h"
#include <cmath>
#include <iostream>
#include <set>
#include <string>

using namespace funes::practice;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const char* kExercise = R"yaml(
id: hum_scale
kind: vocal
category: warmup
level: 1
title: {es: "Escala", en: "Scale"}
description: {es: "d", en: "d"}
steps: {es: ["a"], en: ["a"]}
mistakes: {es: ["m"], en: ["m"]}
sound: "mm"
tempo: 96
pattern: "0 2 4 5 7 5 4 2 0:2"
range: comfortable
roots: {step: 1, direction: up_down}
articulation: legato
dynamics: soft
measure: [pitch, stability]
)yaml";

int test_note_names() {
    CHECK(note_midi("C4") == 60);
    CHECK(note_midi("A4") == 69);
    CHECK(note_midi("A2") == 45);
    CHECK(note_midi("Bb3") == 58);
    CHECK(note_midi("C#4") == 61);
    CHECK(note_midi("G2") == 43);
    CHECK(note_midi("C-1") == 0);
    CHECK(note_midi("H4") == -1);
    CHECK(note_midi("C") == -1);
    CHECK(note_midi("C4x") == -1);
    CHECK(note_midi("") == -1);
    CHECK(midi_name(60) == "C4");
    CHECK(midi_name(61) == "C#4");
    CHECK(midi_name(45) == "A2");
    return 0;
}

int test_pattern() {
    std::vector<Step> s;
    CHECK(parse_pattern("0 2 4:2", s).empty());
    CHECK(s.size() == 3);
    CHECK(s[0].kind == Step::Note && s[0].from == 0 && s[0].beats == 1.0);
    CHECK(s[2].from == 4 && s[2].beats == 2.0);

    CHECK(parse_pattern("0~12:3 r:0.5 -2", s).empty());
    CHECK(s.size() == 3);
    CHECK(s[0].kind == Step::Glide && s[0].from == 0 && s[0].to == 12 && s[0].beats == 3.0);
    CHECK(s[1].kind == Step::Rest && s[1].beats == 0.5);
    CHECK(s[2].from == -2);

    CHECK(!parse_pattern("", s).empty());
    CHECK(!parse_pattern("r:1", s).empty());          // nothing to sing
    CHECK(!parse_pattern("0 x 4", s).empty());
    CHECK(!parse_pattern("0:0", s).empty());           // zero length
    CHECK(!parse_pattern("0:-1", s).empty());
    CHECK(!parse_pattern("0~:2", s).empty());
    CHECK(!parse_pattern("0 40", s).empty());          // beyond three octaves
    return 0;
}

int test_roots() {
    Exercise e;
    CHECK(parse_exercise(kExercise, e).empty());
    Range baritone{note_midi("A2"), note_midi("D4")};   // 45..62
    auto r = roots(e, baritone);
    // Pattern spans 0..7: roots 45..55, up then back down without repeating 55.
    CHECK(!r.empty());
    CHECK(r.front() == 45);
    CHECK(r.back() == 45);
    int top = 0;
    for (int x : r) top = std::max(top, x);
    CHECK(top == 55);
    CHECK(r.size() == 11 + 10);

    e.direction = "down";
    r = roots(e, baritone);
    CHECK(r.front() == 55 && r.back() == 45 && r.size() == 11);

    e.direction = "up";
    e.root_step = 2;
    r = roots(e, baritone);
    CHECK(r.front() == 45 && r.back() == 55 && r.size() == 6);

    // A pattern wider than the range gives no roots, not a wrong one.
    Range narrow{60, 64};
    CHECK(roots(e, narrow).empty());
    return 0;
}

int test_exercise_validation() {
    Exercise e;
    CHECK(parse_exercise(kExercise, e).empty());
    CHECK(e.id == "hum_scale");
    CHECK(e.pitched);
    CHECK(e.tempo == 96);
    CHECK(e.pattern.size() == 9);
    CHECK(e.measure.size() == 2);
    CHECK(e.title.value("es", "") == "Escala");

    auto broken = [&](const std::string& from, const std::string& to) {
        std::string y = kExercise;
        auto at = y.find(from);
        if (at == std::string::npos) return std::string("test bug: ") + from;
        y.replace(at, from.size(), to);
        Exercise x;
        return parse_exercise(y, x);
    };
    CHECK(!broken("category: warmup", "category: dancing").empty());
    CHECK(!broken("level: 1", "level: 7").empty());
    CHECK(!broken("tempo: 96", "tempo: 0").empty());
    CHECK(!broken("range: comfortable", "range: everything").empty());
    CHECK(!broken("direction: up_down", "direction: sideways").empty());
    CHECK(!broken("measure: [pitch, stability]", "measure: [vibes]").empty());
    CHECK(!broken("measure: [pitch, stability]", "measure: []").empty());
    CHECK(!broken("title: {es: \"Escala\", en: \"Scale\"}", "title: {es: \"Escala\"}").empty());
    CHECK(!broken("pattern: \"0 2 4 5 7 5 4 2 0:2\"", "pattern: \"0 2 q\"").empty());
    CHECK(!broken("kind: vocal", "kind: banjo").empty());
    CHECK(!parse_exercise("[1, 2", e).empty());

    const char* unpitched = R"yaml(
id: breath_hiss
kind: vocal
category: breath
level: 1
title: {es: "s", en: "s"}
description: {es: "d", en: "d"}
steps: {es: ["a"], en: ["a"]}
mistakes: {es: ["m"], en: ["m"]}
sound: "sss"
pitched: false
seconds: 20
repeats: 3
measure: [duration]
)yaml";
    CHECK(parse_exercise(unpitched, e).empty());
    CHECK(!e.pitched && e.seconds == 20 && e.repeats == 3);
    CHECK(roots(e, Range{45, 62}).empty());
    std::string y = unpitched;
    y.replace(y.find("seconds: 20"), 11, "seconds: 0");
    CHECK(!parse_exercise(y, e).empty());
    return 0;
}

int test_render() {
    Exercise e;
    CHECK(parse_exercise(kExercise, e).empty());
    auto j = render(e, 45);
    // Nine steps in absolute MIDI, starting on the root, timed in seconds.
    CHECK(j["root"] == "A2");
    CHECK(j["notes"].size() == 9);
    CHECK(j["notes"][0]["midi"] == 45);
    CHECK(j["notes"][4]["midi"] == 52);
    CHECK(j["notes"][4]["name"] == "E3");
    double beat = 60.0 / 96;
    CHECK(std::abs(j["notes"][1]["start"].get<double>() - beat) < 1e-9);
    CHECK(std::abs(j["notes"][8]["seconds"].get<double>() - 2 * beat) < 1e-9);
    return 0;
}

int test_shipped_library(const std::string& dir) {
    Library lib = load_library(dir);
    for (const auto& err : lib.errors) std::cerr << "practice: " << err << "\n";
    CHECK(lib.errors.empty());
    CHECK(lib.exercises.size() >= 15);
    CHECK(lib.voices.size() >= 6);
    std::set<std::string> ids;
    bool has_baritone = false;
    for (const auto& v : lib.voices) {
        if (v.name == "baritone") has_baritone = true;
        CHECK(v.comfortable.low >= v.full.low && v.comfortable.high <= v.full.high);
    }
    CHECK(has_baritone);
    for (const auto& e : lib.exercises) {
        CHECK(ids.insert(e.id).second);
        if (!e.pitched) continue;
        for (const auto& v : lib.voices) {
            Range r = e.range == "full" ? v.full : v.comfortable;
            if (roots(e, r).size() < 2) {
                std::cerr << e.id << " does not fit " << v.name << "\n";
                return 1;
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int rc = 0;
    rc |= test_note_names();
    rc |= test_pattern();
    rc |= test_roots();
    rc |= test_exercise_validation();
    rc |= test_render();
    if (argc > 1) rc |= test_shipped_library(argv[1]);
    if (rc == 0) std::cout << "test_practice: all passed\n";
    return rc;
}
