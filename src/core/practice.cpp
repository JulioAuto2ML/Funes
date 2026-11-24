// =============================================================================
// src/core/practice.cpp — see practice.h
// =============================================================================

#include "practice.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace funes::practice {

namespace {

// A pattern note further than this from its root is a typo, not an exercise.
constexpr int kMaxOffset = 36;

const std::set<std::string> kKinds = {"vocal"};
const std::set<std::string> kCategories = {
    "breath", "warmup", "technique", "agility", "range", "cooldown"};
const std::set<std::string> kRanges = {"comfortable", "full"};
const std::set<std::string> kDirections = {"up", "down", "up_down"};
const std::set<std::string> kArticulations = {"legato", "staccato"};
const std::set<std::string> kDynamics = {"steady", "messa_di_voce", "soft"};
const std::set<std::string> kMeasures = {
    "pitch", "stability", "glide", "onset", "duration", "dynamics", "range"};
const char* kLocales[] = {"es", "en"};

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    try { out = std::stoi(s); } catch (...) { return false; }
    return true;
}

bool parse_beats(const std::string& s, double& out) {
    if (s.empty()) return false;
    size_t used = 0;
    try { out = std::stod(s, &used); } catch (...) { return false; }
    return used == s.size() && out > 0;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string scalar(const YAML::Node& n, const std::string& key) {
    auto v = n[key];
    return v && v.IsScalar() ? v.as<std::string>() : "";
}

std::string one_of(const YAML::Node& n, const std::string& key,
                   const std::set<std::string>& allowed, std::string& out,
                   bool required = true) {
    std::string v = scalar(n, key);
    if (v.empty()) {
        if (required) return key + " is missing";
        return "";
    }
    if (!allowed.count(v)) return key + ": '" + v + "' is not one of the allowed values";
    out = v;
    return "";
}

std::string whole(const YAML::Node& n, const std::string& key, int& out, int min, int max) {
    std::string v = scalar(n, key);
    if (!parse_int(v, out) || out < min || out > max)
        return key + " is a whole number from " + std::to_string(min) + " to " + std::to_string(max);
    return "";
}

// {es: "...", en: "..."} with both locales; `list` wants arrays of strings.
std::string localized(const YAML::Node& n, const std::string& key, bool list,
                      nlohmann::json& out) {
    auto v = n[key];
    if (!v || !v.IsMap()) return key + " needs es and en";
    out = nlohmann::json::object();
    for (const char* loc : kLocales) {
        auto t = v[loc];
        if (!t) return key + " needs es and en";
        if (list) {
            if (!t.IsSequence() || t.size() == 0) return key + "." + loc + " is a non-empty list";
            auto arr = nlohmann::json::array();
            for (const auto& item : t) {
                if (!item.IsScalar()) return key + "." + loc + " is a list of strings";
                arr.push_back(item.as<std::string>());
            }
            out[loc] = arr;
        } else {
            if (!t.IsScalar() || t.as<std::string>().empty()) return key + "." + loc + " is empty";
            out[loc] = t.as<std::string>();
        }
    }
    return "";
}

std::pair<int, int> span(const std::vector<Step>& steps) {
    int lo = kMaxOffset, hi = -kMaxOffset;
    for (const auto& s : steps) {
        if (s.kind == Step::Rest) continue;
        lo = std::min(lo, s.from);
        hi = std::max(hi, s.from);
        if (s.kind == Step::Glide) {
            lo = std::min(lo, s.to);
            hi = std::max(hi, s.to);
        }
    }
    return {lo, hi};
}

std::string parse_range(const YAML::Node& n, Range& out) {
    if (!n || !n.IsSequence() || n.size() != 2) return "a range is [low, high]";
    out.low = note_midi(n[0].as<std::string>());
    out.high = note_midi(n[1].as<std::string>());
    if (out.low < 0 || out.high < 0) return "not a note name in a range";
    if (out.low >= out.high) return "a range goes from low to high";
    return "";
}

} // namespace

int note_midi(const std::string& name) {
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    if (name.size() < 2) return -1;
    char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (letter < 'A' || letter > 'G') return -1;
    int pc = base[letter - 'A'];
    size_t i = 1;
    if (name[i] == '#') { ++pc; ++i; }
    else if (name[i] == 'b') { --pc; ++i; }
    int octave = 0;
    if (!parse_int(name.substr(i), octave) || octave < -1 || octave > 9) return -1;
    int midi = (octave + 1) * 12 + pc;
    return midi >= 0 && midi <= 127 ? midi : -1;
}

std::string midi_name(int midi) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0 || midi > 127) return "";
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}

std::string parse_pattern(const std::string& text, std::vector<Step>& out) {
    out.clear();
    std::istringstream in(text);
    std::string tok;
    bool sung = false;
    while (in >> tok) {
        Step s;
        std::string body = tok;
        auto colon = tok.find(':');
        if (colon != std::string::npos) {
            body = tok.substr(0, colon);
            if (!parse_beats(tok.substr(colon + 1), s.beats))
                return "'" + tok + "': the length after ':' is a positive number of beats";
        }
        auto tilde = body.find('~');
        if (body == "r") {
            s.kind = Step::Rest;
        } else if (tilde != std::string::npos) {
            s.kind = Step::Glide;
            if (!parse_int(body.substr(0, tilde), s.from) || !parse_int(body.substr(tilde + 1), s.to))
                return "'" + tok + "': a glide is from~to in semitones";
        } else if (!parse_int(body, s.from)) {
            return "'" + tok + "': a step is semitones from the root, a~b or r";
        }
        if (s.kind != Step::Rest) {
            sung = true;
            if (std::abs(s.from) > kMaxOffset || std::abs(s.to) > kMaxOffset)
                return "'" + tok + "': more than three octaves from the root";
        }
        out.push_back(s);
    }
    if (out.empty()) return "the pattern is empty";
    if (!sung) return "the pattern has nothing to sing";
    return "";
}

std::string parse_exercise(const std::string& yaml_text, Exercise& out) {
    YAML::Node n;
    try {
        n = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        return std::string("not YAML: ") + e.what();
    }
    if (!n.IsMap()) return "an exercise is a mapping";
    Exercise e;
    e.id = scalar(n, "id");
    if (e.id.empty()) return "id is missing";
    std::string err;
    if (!(err = one_of(n, "kind", kKinds, e.kind)).empty()) return err;
    if (!(err = one_of(n, "category", kCategories, e.category)).empty()) return err;
    if (!(err = whole(n, "level", e.level, 1, 3)).empty()) return err;
    if (!(err = localized(n, "title", false, e.title)).empty()) return err;
    if (!(err = localized(n, "description", false, e.description)).empty()) return err;
    if (!(err = localized(n, "steps", true, e.steps)).empty()) return err;
    if (!(err = localized(n, "mistakes", true, e.mistakes)).empty()) return err;
    e.sound = scalar(n, "sound");
    if (e.sound.empty()) return "sound is missing";

    auto measure = n["measure"];
    if (!measure || !measure.IsSequence() || measure.size() == 0)
        return "measure lists at least one thing to score";
    for (const auto& m : measure) {
        std::string v = m.IsScalar() ? m.as<std::string>() : "";
        if (!kMeasures.count(v)) return "measure: '" + v + "' is not something the browser measures";
        e.measure.push_back(v);
    }

    auto pitched = n["pitched"];
    e.pitched = !pitched || pitched.as<std::string>() != "false";
    if (!e.pitched) {
        if (!(err = whole(n, "seconds", e.seconds, 1, 120)).empty()) return err;
        if (!(err = whole(n, "repeats", e.repeats, 1, 20)).empty()) return err;
        out = std::move(e);
        return "";
    }

    if (!(err = whole(n, "tempo", e.tempo, 30, 240)).empty()) return err;
    e.pattern_text = scalar(n, "pattern");
    if (!(err = parse_pattern(e.pattern_text, e.pattern)).empty()) return "pattern: " + err;
    if (!(err = one_of(n, "range", kRanges, e.range)).empty()) return err;
    auto r = n["roots"];
    if (!r || !r.IsMap()) return "roots is {step, direction}";
    if (!(err = whole(r, "step", e.root_step, 1, 12)).empty()) return "roots." + err;
    if (!(err = one_of(r, "direction", kDirections, e.direction)).empty()) return "roots." + err;
    if (!(err = one_of(n, "articulation", kArticulations, e.articulation, false)).empty()) return err;
    if (!(err = one_of(n, "dynamics", kDynamics, e.dynamics, false)).empty()) return err;
    out = std::move(e);
    return "";
}

std::string parse_voices(const std::string& yaml_text, std::vector<Voice>& out) {
    out.clear();
    YAML::Node n;
    try {
        n = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        return std::string("not YAML: ") + e.what();
    }
    auto voices = n["voices"];
    if (!voices || !voices.IsMap()) return "voices is a mapping of name → ranges";
    for (const auto& kv : voices) {
        Voice v;
        v.name = kv.first.as<std::string>();
        std::string err = parse_range(kv.second["full"], v.full);
        if (err.empty()) err = parse_range(kv.second["comfortable"], v.comfortable);
        if (!err.empty()) return v.name + ": " + err;
        if (v.comfortable.low < v.full.low || v.comfortable.high > v.full.high)
            return v.name + ": comfortable lies inside full";
        out.push_back(v);
    }
    if (out.empty()) return "no voices";
    return "";
}

std::vector<int> roots(const Exercise& e, Range r) {
    std::vector<int> up;
    if (!e.pitched || e.pattern.empty()) return up;
    auto [lo, hi] = span(e.pattern);
    int first = r.low - lo, last = r.high - hi;
    for (int root = first; root <= last; root += e.root_step) up.push_back(root);
    if (e.direction == "down") return {up.rbegin(), up.rend()};
    if (e.direction == "up_down" && up.size() > 1)
        up.insert(up.end(), up.rbegin() + 1, up.rend());
    return up;
}

nlohmann::json render(const Exercise& e, int root) {
    double beat = 60.0 / (e.tempo > 0 ? e.tempo : 60);
    auto notes = nlohmann::json::array();
    double t = 0;
    for (const auto& s : e.pattern) {
        double len = s.beats * beat;
        if (s.kind != Step::Rest) {
            nlohmann::json n = {
                {"kind", s.kind == Step::Glide ? "glide" : "note"},
                {"midi", root + s.from},
                {"name", midi_name(root + s.from)},
                {"start", t},
                {"seconds", len},
            };
            if (s.kind == Step::Glide) {
                n["to"] = root + s.to;
                n["to_name"] = midi_name(root + s.to);
            }
            notes.push_back(n);
        }
        t += len;
    }
    return {{"root", midi_name(root)}, {"root_midi", root}, {"seconds", t}, {"notes", notes}};
}

nlohmann::json Exercise::to_json() const {
    nlohmann::json j = {
        {"id", id}, {"kind", kind}, {"category", category}, {"level", level},
        {"title", title}, {"description", description}, {"steps", steps},
        {"mistakes", mistakes}, {"sound", sound}, {"pitched", pitched},
        {"measure", measure},
    };
    if (pitched) {
        j["tempo"] = tempo;
        j["pattern"] = pattern_text;
        j["range"] = range;
        j["roots"] = {{"step", root_step}, {"direction", direction}};
        j["articulation"] = articulation;
        j["dynamics"] = dynamics;
    } else {
        j["seconds"] = seconds;
        j["repeats"] = repeats;
    }
    return j;
}

namespace {

// What a human voice can be: E1 (a basso profondo's floor) to C7 (a
// soprano's whistle). A range outside it is a measuring error.
constexpr int kLowestVoice = 28, kHighestVoice = 96;
// Narrower than an octave, the range check missed something: the octave
// exercises would have no root, and the person would be shown nothing.
constexpr int kNarrowestRange = 12;

std::string range_from_json(const nlohmann::json& j, Range& out) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_string() || !j[1].is_string())
        return "a range is [low, high] as note names";
    out.low = note_midi(j[0].get<std::string>());
    out.high = note_midi(j[1].get<std::string>());
    if (out.low < 0 || out.high < 0) return "not a note name in a range";
    if (out.low >= out.high) return "a range goes from low to high";
    return "";
}

nlohmann::json range_to_json(Range r) {
    return nlohmann::json::array({midi_name(r.low), midi_name(r.high)});
}

} // namespace

Range comfortable_within(Range full) {
    if (full.high - full.low < 17) return full;
    return {full.low + 2, full.high - 3};
}

std::string profile_from_json(const nlohmann::json& j, Profile& out) {
    if (!j.is_object()) return "a profile is an object";
    Profile p;
    if (auto err = range_from_json(j.value("full", nlohmann::json()), p.full); !err.empty())
        return "full: " + err;
    if (p.full.low < kLowestVoice || p.full.high > kHighestVoice)
        return "full: outside what a voice can sing (E1 to C7)";
    if (p.full.high - p.full.low < kNarrowestRange)
        return "full: narrower than an octave — measure again";
    if (j.contains("comfortable")) {
        if (auto err = range_from_json(j["comfortable"], p.comfortable); !err.empty())
            return "comfortable: " + err;
        if (p.comfortable.low < p.full.low || p.comfortable.high > p.full.high)
            return "comfortable lies inside full";
    } else {
        p.comfortable = comfortable_within(p.full);
    }
    p.voice = j.value("voice", "");
    p.source = j.value("source", "default");
    if (p.source != "default" && p.source != "measured")
        return "source is default or measured";
    p.measured_at = j.value("measured_at", "");
    if (p.voice.size() > 32 || p.measured_at.size() > 32) return "voice and measured_at are short";
    out = std::move(p);
    return "";
}

nlohmann::json profile_to_json(const Profile& p) {
    nlohmann::json j = {
        {"voice", p.voice}, {"source", p.source},
        {"full", range_to_json(p.full)}, {"comfortable", range_to_json(p.comfortable)},
    };
    if (!p.measured_at.empty()) j["measured_at"] = p.measured_at;
    return j;
}

Profile profile_for(const Voice& v) {
    Profile p;
    p.voice = v.name;
    p.full = v.full;
    p.comfortable = v.comfortable;
    p.source = "default";
    return p;
}

nlohmann::json plan(const Exercise& e, const Profile& p) {
    Range r = e.range == "full" ? p.full : p.comfortable;
    auto reps = nlohmann::json::array();
    for (int root : roots(e, r)) reps.push_back(render(e, root));
    return {{"exercise", e.to_json()}, {"range", range_to_json(r)}, {"reps", reps}};
}

Library load_library(const std::string& dir) {
    Library lib;
    std::error_code ec;
    fs::path root(dir);
    if (!fs::is_directory(root, ec)) {
        lib.errors.push_back(dir + ": not a directory");
        return lib;
    }
    std::string err = parse_voices(read_file(root / "voices.yaml"), lib.voices);
    if (!err.empty()) lib.errors.push_back("voices.yaml: " + err);

    for (const auto& kind : fs::directory_iterator(root, ec)) {
        if (!kind.is_directory()) continue;
        for (const auto& f : fs::directory_iterator(kind.path(), ec)) {
            if (f.path().extension() != ".yaml") continue;
            std::string where = kind.path().filename().string() + "/" + f.path().filename().string();
            Exercise e;
            err = parse_exercise(read_file(f.path()), e);
            if (err.empty() && e.id != f.path().stem().string())
                err = "id '" + e.id + "' is not the file's name";
            if (err.empty() && e.kind != kind.path().filename().string())
                err = "kind '" + e.kind + "' is not the directory's name";
            if (!err.empty()) {
                lib.errors.push_back(where + ": " + err);
                continue;
            }
            lib.exercises.push_back(std::move(e));
        }
    }
    std::sort(lib.exercises.begin(), lib.exercises.end(),
              [](const Exercise& a, const Exercise& b) { return a.id < b.id; });
    return lib;
}

} // namespace funes::practice
