// =============================================================================
// src/core/practice.h — practice exercise libraries (practice/)
// =============================================================================
// A practice session is exercises a person does while the browser measures
// them; practice/README.md has the file format. The split this feature rests
// on: the browser measures (pitch from the microphone, never sent anywhere),
// the code scores, a model only coaches — it picks exercises and their
// parameters and comments on the numbers. It never writes notes: a local model
// writing a scale gets it wrong, and a wrong note is one a person then sings.
// So notes come only from these files, and this is where they are checked.
//
// An exercise's notes are a pattern of semitones from a root. The roots it is
// sung on are computed from the singer's range (roots()), so one file serves a
// bass and a soprano; render() turns one root into absolute notes with times,
// which is all the browser needs to play and to score against.
// =============================================================================

#pragma once
#include "json.hpp"
#include <string>
#include <vector>

namespace funes::practice {

// Scientific pitch ("C4" = 60, "A4" = 69, "Bb3", "C#4") → MIDI; -1 if not one.
int note_midi(const std::string& name);
// MIDI → name, sharps: 61 → "C#4".
std::string midi_name(int midi);

struct Step {
    enum Kind { Note, Glide, Rest };
    Kind   kind = Note;
    int    from = 0;      // semitones from the root
    int    to = 0;        // Glide only
    double beats = 1.0;
};

// "0 2 4:2 0~12:3 r:1" → steps. "" or what is wrong with it.
std::string parse_pattern(const std::string& text, std::vector<Step>& out);

struct Range { int low = 0, high = 0; };   // MIDI, inclusive

struct Voice {
    std::string name;
    Range full, comfortable;
};

struct Exercise {
    std::string id, kind, category, sound;
    int level = 1;
    // Localized: {"es": ..., "en": ...}; strings for title/description,
    // arrays of strings for steps/mistakes.
    nlohmann::json title, description, steps, mistakes;
    bool pitched = true;
    // pitched
    int tempo = 0;                       // quarter notes per minute
    std::vector<Step> pattern;
    std::string pattern_text;
    std::string range = "comfortable";   // comfortable | full
    int root_step = 1;
    std::string direction = "up";        // up | down | up_down
    std::string articulation = "legato";
    std::string dynamics = "steady";
    // unpitched
    int seconds = 0, repeats = 1;
    std::vector<std::string> measure;

    nlohmann::json to_json() const;
};

// One exercise file → exercise. "" or the first thing wrong with it.
std::string parse_exercise(const std::string& yaml_text, Exercise& out);

// voices.yaml → voices. "" or the first thing wrong with it.
std::string parse_voices(const std::string& yaml_text, std::vector<Voice>& out);

// The roots `e` is sung on within `r`, in singing order: the lowest root puts
// the pattern's lowest note on r.low, the highest its top note on r.high.
// Empty for an unpitched exercise or a pattern wider than the range.
std::vector<int> roots(const Exercise& e, Range r);

// One repetition on `root`: {"root", "notes": [{midi, name, to?, start,
// seconds, kind}]}, times in seconds from the exercise's tempo.
nlohmann::json render(const Exercise& e, int root);

// A singer's range: from voices.yaml until the range check measures it.
struct Profile {
    std::string voice;                // a voices.yaml name, or "" when measured
    Range full, comfortable;
    std::string source = "default";   // default | measured
    std::string measured_at;          // ISO date, when measured
};

// {voice?, full: [low, high], comfortable?: [low, high], source?, measured_at?}
// → profile; comfortable defaults to comfortable_within(full). "" or what is
// wrong — a range a person could not sing (outside E1..C7, under an octave wide)
// is refused rather than fitted.
std::string profile_from_json(const nlohmann::json& j, Profile& out);
nlohmann::json profile_to_json(const Profile& p);
Profile profile_for(const Voice& v);

// The tessitura inside a measured range: a whole tone above the bottom and a
// minor third below the top, where a voice works without effort.
Range comfortable_within(Range full);

// An exercise fitted to a singer: {exercise, range: [low, high], reps: [render
// of each root in order]}. Unpitched exercises have no reps.
nlohmann::json plan(const Exercise& e, const Profile& p);

struct Library {
    std::vector<Voice> voices;
    std::vector<Exercise> exercises;
    std::vector<std::string> errors;     // one per file that did not load
};

// <dir>/voices.yaml and every <dir>/<kind>/*.yaml, sorted by id. A file that
// does not parse is left out and named in `errors`, so one typo does not
// empty the library.
Library load_library(const std::string& dir);

} // namespace funes::practice
