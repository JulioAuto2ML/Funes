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

int test_profile() {
    Profile p;
    // A measured range: comfortable is derived, a little inside it.
    CHECK(profile_from_json({{"full", {"G2", "G4"}}, {"source", "measured"}}, p).empty());
    CHECK(p.full.low == 43 && p.full.high == 67);
    CHECK(p.comfortable.low == 45 && p.comfortable.high == 64);
    CHECK(p.source == "measured");
    auto j = profile_to_json(p);
    CHECK(j["full"][0] == "G2" && j["comfortable"][1] == "E4");
    Profile back;
    CHECK(profile_from_json(j, back).empty());
    CHECK(back.comfortable.low == p.comfortable.low && back.comfortable.high == p.comfortable.high);

    // An explicit comfortable range is kept, if it lies inside full.
    CHECK(profile_from_json({{"voice", "baritone"}, {"full", {"G2", "G4"}},
                             {"comfortable", {"A2", "D4"}}}, p).empty());
    CHECK(p.voice == "baritone" && p.comfortable.high == 62);
    CHECK(!profile_from_json({{"full", {"G2", "G4"}}, {"comfortable", {"F2", "D4"}}}, p).empty());

    CHECK(!profile_from_json({{"full", {"G4", "G2"}}}, p).empty());     // upside down
    CHECK(!profile_from_json({{"full", {"C4", "F4"}}}, p).empty());     // too narrow to sing a fifth
    CHECK(!profile_from_json({{"full", {"C0", "C4"}}}, p).empty());     // not a human voice
    CHECK(!profile_from_json({{"full", "G2-G4"}}, p).empty());
    CHECK(!profile_from_json({{"full", {"G2", "G4"}}, {"source", "guess"}}, p).empty());
    CHECK(!profile_from_json(nlohmann::json::array(), p).empty());

    // A default voice becomes a profile.
    Voice v{"baritone", {43, 67}, {45, 62}};
    p = profile_for(v);
    CHECK(p.voice == "baritone" && p.source == "default" && p.comfortable.high == 62);
    return 0;
}

int test_plan() {
    Exercise e;
    CHECK(parse_exercise(kExercise, e).empty());
    Profile p = profile_for(Voice{"baritone", {43, 67}, {45, 62}});
    auto j = plan(e, p);
    // A comfortable-range exercise walks the comfortable range.
    CHECK(j["range"][0] == "A2" && j["range"][1] == "D4");
    CHECK(j["reps"].size() == 21);
    CHECK(j["reps"][0]["root"] == "A2");
    CHECK(j["exercise"]["id"] == "hum_scale");

    e.range = "full";
    j = plan(e, p);
    CHECK(j["range"][0] == "G2");
    CHECK(j["reps"][0]["root"] == "G2");
    return 0;
}

// A small library for the coach's reports: one pitched, one breathing.
Library tiny_library() {
    Library lib;
    Exercise e;
    parse_exercise(kExercise, e);
    lib.exercises.push_back(e);
    Exercise hiss;
    hiss.id = "breath_hiss"; hiss.kind = "vocal"; hiss.category = "breath";
    hiss.title = {{"es", "Siseo"}, {"en", "Hiss"}};
    hiss.pitched = false; hiss.seconds = 20; hiss.repeats = 3;
    hiss.measure = {"duration"};
    lib.exercises.push_back(hiss);
    return lib;
}

nlohmann::json session(const std::string& at, const std::string& id, int pitch,
                       std::vector<std::pair<int, int>> off = {}) {
    nlohmann::json notes = nlohmann::json::array();
    for (auto [midi, cents] : off)
        notes.push_back({{"midi", midi}, {"kind", "note"}, {"cents", cents}, {"in_tune", 10}});
    notes.push_back({{"midi", 45}, {"kind", "note"}, {"cents", 2}, {"in_tune", 95}});
    return {{"at", at}, {"exercise", id},
            {"scores", {{"pitch", pitch}, {"stability", 12.5}, {"onset_ms", 180}}},
            {"reps", {{{"root", "A2"}, {"notes", notes}}}}};
}

int test_history_report() {
    Library lib = tiny_library();
    Profile p = profile_for(Voice{"baritone", {43, 67}, {45, 62}});
    std::vector<nlohmann::json> s = {
        session("2026-11-01T10:00:00Z", "hum_scale", 40),                 // before the window
        session("2026-11-20T10:00:00Z", "hum_scale", 60, {{52, -40}}),
        session("2026-11-24T10:00:00Z", "hum_scale", 80, {{52, -35}, {50, 30}}),
        {{"at", "2026-11-24T10:05:00Z"}, {"exercise", "breath_hiss"},
         {"scores", {{"duration_s", 14.2}, {"duration", 71}, {"stability", 81}}}},
        {{"at", "2026-11-24T10:06:00Z"}, {"exercise", "gone_exercise"}, {"scores", {{"pitch", 1}}}},
    };
    std::string r = history_report(s, lib, &p, "2026-11-10", "es");
    // The window, the range, the trend and the note that keeps going flat.
    CHECK(r.find("Sol2") != std::string::npos);                  // solfège, as the Spanish UI shows it
    CHECK(r.find("3 session") != std::string::npos);            // the unknown id is not counted
    CHECK(r.find("hum_scale") != std::string::npos);
    CHECK(r.find("60% → 80%") != std::string::npos);
    CHECK(r.find("Mi3") != std::string::npos);                  // midi 52, off twice
    CHECK(r.find("-38") != std::string::npos);                  // mean of -40 and -35, rounded down
    CHECK(r.find("14.2 s") != std::string::npos);
    // A breath's stability is evenness in percent, never a wobble in cents.
    CHECK(r.find("evenness 81%") != std::string::npos);
    CHECK(r.find("±81") == std::string::npos);
    CHECK(r.find("gone_exercise") == std::string::npos);
    CHECK(r.find("2026-11-01") == std::string::npos);

    // No sessions: it says so, and still names the range.
    r = history_report({}, lib, &p, "2026-11-10", "es");
    CHECK(r.find("No practice") != std::string::npos);
    CHECK(history_report({}, lib, &p, "2026-11-10", "en").find("G2–G4") != std::string::npos);
    CHECK(history_report({}, lib, nullptr, "2026-11-10", "es").find("No range") != std::string::npos);
    return 0;
}

int test_exercises_report() {
    Library lib = tiny_library();
    Profile p = profile_for(Voice{"baritone", {43, 67}, {45, 62}});
    std::string r = exercises_report(lib, &p, "es");
    CHECK(r.find("hum_scale") != std::string::npos);
    CHECK(r.find("Escala") != std::string::npos);               // the caller's language
    CHECK(r.find("21 rep") != std::string::npos);               // what fits this range
    CHECK(r.find("breath_hiss") != std::string::npos);
    return 0;
}

int test_make_plan() {
    Library lib = tiny_library();
    Profile p = profile_for(Voice{"baritone", {43, 67}, {45, 62}});
    nlohmann::json out;
    nlohmann::json args = {{"items", {{{"id", "breath_hiss"}}, {{"id", "hum_scale"}, {"reps", 8}}}},
                           {"note", "Hoy, aire y afinación en lo grave."}};
    CHECK(make_plan(args, lib, &p, "2026-11-24T10:00:00Z", out).empty());
    CHECK(out["items"].size() == 2);
    CHECK(out["items"][1]["reps"] == 8);
    CHECK(out["items"][0]["reps"] == 3);                        // a breathing exercise: its repeats
    CHECK(out["created_at"] == "2026-11-24T10:00:00Z");
    CHECK(out["note"] == "Hoy, aire y afinación en lo grave.");

    // Reps beyond what fits are capped, not refused: the plan still works.
    args["items"][1]["reps"] = 99;
    CHECK(make_plan(args, lib, &p, "t", out).empty());
    CHECK(out["items"][1]["reps"] == 21);

    // An exercise that does not exist is refused by name — the model invented it.
    std::string err = make_plan({{"items", {{{"id", "yodel_drill"}}}}}, lib, &p, "t", out);
    CHECK(err.find("yodel_drill") != std::string::npos);
    CHECK(!make_plan({{"items", nlohmann::json::array()}}, lib, &p, "t", out).empty());
    CHECK(!make_plan({{"items", {{{"id", "hum_scale"}, {"reps", 0}}}}}, lib, &p, "t", out).empty());
    CHECK(!make_plan({{"items", {{{"id", "hum_scale"}}}}}, lib, nullptr, "t", out).empty());  // no range
    CHECK(!make_plan({{"items", {{{"id", "hum_scale"}}}}, {"note", std::string(2000, 'x')}},
                     lib, &p, "t", out).empty());
    nlohmann::json many = nlohmann::json::array();
    for (int i = 0; i < 13; ++i) many.push_back({{"id", "hum_scale"}});
    CHECK(!make_plan({{"items", many}}, lib, &p, "t", out).empty());
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
    rc |= test_profile();
    rc |= test_plan();
    rc |= test_history_report();
    rc |= test_exercises_report();
    rc |= test_make_plan();
    if (argc > 1) rc |= test_shipped_library(argv[1]);
    if (rc == 0) std::cout << "test_practice: all passed\n";
    return rc;
}
