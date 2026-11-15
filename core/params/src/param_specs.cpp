#include "amq/params/param_spec.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/conventions.hpp"
#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

namespace amq::params {

namespace {

ParamSpec continuous(std::string id, std::string name, std::string group, Unit unit, double min, double max,
                     double def, bool provisional = false, bool automatable = true) {
    ParamSpec s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.group = std::move(group);
    s.type = ParamType::Continuous;
    s.unit = unit;
    s.min = min;
    s.max = max;
    s.def = def;
    s.provisional = provisional;
    s.automatable = automatable;
    return s;
}

ParamSpec toggle(std::string id, std::string name, std::string group, bool def, bool automatable = true) {
    ParamSpec s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.group = std::move(group);
    s.type = ParamType::Toggle;
    s.min = 0.0;
    s.max = 1.0;
    s.def = def ? 1.0 : 0.0;
    s.automatable = automatable;
    return s;
}

ParamSpec choice(std::string id, std::string name, std::string group, std::vector<std::string> labels, int def,
                 bool provisional = false, bool automatable = true) {
    ParamSpec s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.group = std::move(group);
    s.type = ParamType::Choice;
    s.choices = std::move(labels);
    s.min = 0.0;
    s.max = static_cast<double>(s.choices.size() - 1);
    s.def = def;
    s.provisional = provisional;
    s.automatable = automatable;
    return s;
}

const double kLog2FMin = std::log2(dsp::kMinBandFrequencyHz);
const double kLog2FMax = std::log2(dsp::kMaxBandFrequencyHz);
const double kLog2ScMax = std::log2(20000.0);

std::vector<std::string> shapeLabels() {
    std::vector<std::string> v;
    for (int i = 0; i < dsp::kShapeCount; ++i) v.emplace_back(dsp::shapeName(static_cast<dsp::Shape>(i)));
    return v;
}

std::vector<std::string> slopeLabels() {
    std::vector<std::string> v;
    for (int i = 0; i < dsp::kSlopeCount; ++i) v.emplace_back(dsp::slopeName(static_cast<dsp::Slope>(i)));
    return v;
}

void addBand(std::vector<ParamSpec>& v, int b) {
    char pre[16], grp[16];
    std::snprintf(pre, sizeof(pre), "band%02d.", b + 1);
    std::snprintf(grp, sizeof(grp), "Band %d", b + 1);
    const std::string p = pre, g = grp, n = g + " ";
    // Order must match BandField.
    v.push_back(toggle(p + "used", n + "Used", g, false));
    v.push_back(toggle(p + "enabled", n + "Enabled", g, true));
    v.push_back(continuous(p + "freq", n + "Frequency", g, Unit::Log2Hz, kLog2FMin, kLog2FMax,
                           std::log2(dsp::kDefaultBandFrequencyHz)));
    v.push_back(continuous(p + "gain", n + "Gain", g, Unit::Db, -30.0, 30.0, 0.0));
    v.push_back(continuous(p + "q", n + "Q", g, Unit::QPosition, 0.0, 1.0, 0.5));
    v.push_back(choice(p + "shape", n + "Shape", g, shapeLabels(), static_cast<int>(dsp::Shape::Bell)));
    v.push_back(choice(p + "slope", n + "Slope", g, slopeLabels(), static_cast<int>(dsp::Slope::Db12), true));
    v.push_back(choice(p + "placement", n + "Placement", g, {"Stereo", "Left", "Right", "Mid", "Side"}, 0));
    v.push_back(continuous(p + "dynRange", n + "Dynamic Range", g, Unit::Db, -30.0, 30.0, 0.0));
    v.push_back(toggle(p + "dynEnabled", n + "Dynamic", g, false));
    v.push_back(toggle(p + "dynAuto", n + "Auto Threshold", g, true));
    // Range per area 01 R8; the default is PROVISIONAL (DSP lead).
    v.push_back(continuous(p + "threshold", n + "Threshold", g, Unit::Db, -90.0, 0.0, -20.0, true));
    v.push_back(continuous(p + "attack", n + "Attack", g, Unit::Knob, 0.0, 100.0, 50.0));
    v.push_back(continuous(p + "release", n + "Release", g, Unit::Knob, 0.0, 100.0, 50.0));
    v.push_back(toggle(p + "scExternal", n + "External Side Chain", g, false));
    v.push_back(choice(p + "scMode", n + "Side Chain Mode", g, {"Band", "Free"}, 0));
    // Side-chain filter 10 Hz - 20 kHz (area 01 R8), fully open by default (area 21 R8).
    v.push_back(continuous(p + "scLow", n + "Side Chain Low", g, Unit::Log2Hz, kLog2FMin, kLog2ScMax, kLog2FMin));
    v.push_back(continuous(p + "scHigh", n + "Side Chain High", g, Unit::Log2Hz, kLog2FMin, kLog2ScMax, kLog2ScMax));
    v.push_back(toggle(p + "scListen", n + "Side Chain Listen", g, false));
    v.push_back(toggle(p + "spectral", n + "Spectral", g, false));
    v.push_back(continuous(p + "spectralDensity", n + "Spectral Density", g, Unit::Knob, 0.0, 100.0, 50.0));
    v.push_back(toggle(p + "spectralTilt", n + "Spectral Tilt", g, false));
    v.push_back(toggle(p + "solo", n + "Solo", g, false));
}

void addGlobals(std::vector<ParamSpec>& v) {
    // Order must match GlobalParam.
    v.push_back(choice("global.mode", "Processing Mode", "Global", {"Zero Latency", "Natural Phase", "Linear Phase"}, 0));
    v.push_back(choice("global.resolution", "Resolution", "Global", {"Low", "Medium", "High", "Very High", "Maximum"}, 1,
                       true));
    v.push_back(continuous("global.gainScale", "Gain Scale", "Global", Unit::Percent, 0.0, 200.0, 100.0));
    v.push_back(continuous("global.outputLevel", "Output Level", "Output", Unit::OutputLevelDb, kOutputSilenceDb,
                           kOutputLevelMaxDb, 0.0, true));
    v.push_back(continuous("global.outputPan", "Output Pan", "Output", Unit::Pan, -100.0, 100.0, 0.0, true));
    v.push_back(choice("global.panMode", "Pan Mode", "Output", {"L/R", "M/S"}, 0));
    v.push_back(toggle("global.invert", "Invert Phase", "Output", false));
    auto bypass = toggle("global.bypass", "Bypass", "Global", false);
    bypass.isBypass = true;
    v.push_back(bypass);
    v.push_back(toggle("global.autoGain", "Auto Gain", "Output", false));
    v.push_back(continuous("global.soloGain", "Solo Gain", "Global", Unit::Db, -20.0, 20.0, 0.0));
    // Analyzer display settings: saved with the state, not automatable (area 12 R15, area 21 R2).
    v.push_back(toggle("analyzer.showPre", "Analyzer Pre", "Analyzer", true, false));
    v.push_back(toggle("analyzer.showPost", "Analyzer Post", "Analyzer", true, false));
    v.push_back(toggle("analyzer.showExternal", "Analyzer External", "Analyzer", false, false));
    v.push_back(choice("analyzer.range", "Analyzer Range", "Analyzer", {"60 dB", "90 dB", "120 dB"}, 1, true, false));
    v.push_back(choice("analyzer.resolution", "Analyzer Resolution", "Analyzer", {"Low", "Medium", "High", "Maximum"}, 1,
                       true, false));
    v.push_back(choice("analyzer.speed", "Analyzer Speed", "Analyzer", {"Very Slow", "Slow", "Medium", "Fast", "Very Fast"},
                       2, true, false));
    v.push_back(continuous("analyzer.tilt", "Analyzer Tilt", "Analyzer", Unit::DbPerOctave, 0.0, 6.0, 4.5, true, false));
    v.push_back(toggle("analyzer.freeze", "Analyzer Freeze", "Analyzer", false, false));
    v.push_back(toggle("analyzer.collisions", "Analyzer Collisions", "Analyzer", false, false));
    v.push_back(toggle("analyzer.grab", "Spectrum Grab", "Analyzer", true, false));
    v.push_back(choice("display.range", "Display Range", "Analyzer", {"3 dB", "6 dB", "12 dB", "30 dB"}, 2, true, false));
}

struct Registry {
    std::vector<ParamSpec> specs;
    std::unordered_map<std::string_view, ParamIndex> byId;
    std::unordered_map<uint32_t, ParamIndex> byHost;

    Registry() {
        specs.reserve(kParamCount);
        for (int b = 0; b < kBandCount; ++b) addBand(specs, b);
        addGlobals(specs);
        if (specs.size() != kParamCount) throw std::logic_error("parameter table size mismatch");
        for (ParamIndex i = 0; i < specs.size(); ++i) {
            auto& s = specs[i];
            s.hostId = stableHostId(s.id);
            if (!byId.emplace(s.id, i).second) throw std::logic_error("duplicate parameter id " + s.id);
            if (!byHost.emplace(s.hostId, i).second) throw std::logic_error("host id collision at " + s.id);
        }
        for (const auto& r : reservedIds())
            if (byId.count(r) || byHost.count(stableHostId(r))) throw std::logic_error("reserved id clash " + r);
    }
};

const Registry& registry() {
    static const Registry r;
    return r;
}

double roundIndex(double v) { return std::floor(v + 0.5); }

}  // namespace

const std::vector<std::string>& reservedIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v{"global.character", "global.midiReceive"};
        char buf[32];
        for (int b = 0; b < kBandCount; ++b) {
            std::snprintf(buf, sizeof(buf), "band%02d.speakers", b + 1);
            v.emplace_back(buf);
        }
        return v;
    }();
    return ids;
}

const std::vector<ParamSpec>& parameterSpecs() { return registry().specs; }

const ParamSpec* specAt(ParamIndex index) noexcept {
    const auto& s = registry().specs;
    return index < s.size() ? &s[index] : nullptr;
}

std::optional<ParamIndex> findById(std::string_view id) noexcept {
    const auto& m = registry().byId;
    const auto it = m.find(id);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

std::optional<ParamIndex> findByHostId(uint32_t hostId) noexcept {
    const auto& m = registry().byHost;
    const auto it = m.find(hostId);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

double clampPlain(const ParamSpec& s, double plain) noexcept {
    if (std::isnan(plain)) return s.def;
    plain = std::clamp(plain, s.min, s.max);
    return s.type == ParamType::Continuous ? plain : roundIndex(plain);
}

double hostMin(const ParamSpec& s) noexcept { return s.unit == Unit::OutputLevelDb ? 0.0 : s.min; }
double hostMax(const ParamSpec& s) noexcept { return s.unit == Unit::OutputLevelDb ? 1.0 : s.max; }

double toHost(const ParamSpec& s, double plain) noexcept {
    plain = clampPlain(s, plain);
    return s.unit == Unit::OutputLevelDb ? outputLevelPositionFromDb(plain) : plain;
}

double fromHost(const ParamSpec& s, double hostValue) noexcept {
    if (std::isnan(hostValue)) return s.def;
    if (s.unit == Unit::OutputLevelDb) return clampPlain(s, outputLevelDbFromPosition(hostValue));
    return clampPlain(s, hostValue);
}

double toNormalised(const ParamSpec& s, double plain) noexcept {
    const double lo = hostMin(s), hi = hostMax(s);
    return hi > lo ? (toHost(s, plain) - lo) / (hi - lo) : 0.0;
}

double fromNormalised(const ParamSpec& s, double n) noexcept {
    n = std::clamp(std::isnan(n) ? 0.0 : n, 0.0, 1.0);
    return fromHost(s, hostMin(s) + n * (hostMax(s) - hostMin(s)));
}

// ---------------------------------------------------------------------------
// Display text
// ---------------------------------------------------------------------------

std::string formatPlain(const ParamSpec& s, double plain) {
    plain = clampPlain(s, plain);
    char buf[64];
    switch (s.type) {
        case ParamType::Toggle: return plain >= 0.5 ? "On" : "Off";
        case ParamType::Choice: return s.choices[static_cast<size_t>(plain)];
        case ParamType::Continuous: break;
    }
    switch (s.unit) {
        case Unit::Log2Hz: {
            const double hz = std::exp2(plain);
            if (hz >= 10000.0) std::snprintf(buf, sizeof(buf), "%.1f kHz", hz / 1000.0);
            else if (hz >= 1000.0) std::snprintf(buf, sizeof(buf), "%.2f kHz", hz / 1000.0);
            else if (hz >= 100.0) std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
            else std::snprintf(buf, sizeof(buf), "%.1f Hz", hz);
            break;
        }
        case Unit::Db: std::snprintf(buf, sizeof(buf), "%+.2f dB", plain); break;
        case Unit::QPosition: std::snprintf(buf, sizeof(buf), "%.3g", qFromPosition(plain)); break;
        case Unit::Percent: std::snprintf(buf, sizeof(buf), "%.1f %%", plain); break;
        case Unit::Knob: std::snprintf(buf, sizeof(buf), "%.1f", plain); break;
        case Unit::Pan:
            if (std::abs(plain) < 0.05) return "C";
            std::snprintf(buf, sizeof(buf), "%.1f %c", std::abs(plain), plain < 0 ? 'L' : 'R');
            break;
        case Unit::OutputLevelDb:
            if (plain <= kOutputSilenceDb) return "-inf dB";
            std::snprintf(buf, sizeof(buf), "%+.2f dB", plain);
            break;
        case Unit::DbPerOctave: std::snprintf(buf, sizeof(buf), "%.1f dB/oct", plain); break;
        case Unit::None: std::snprintf(buf, sizeof(buf), "%g", plain); break;
    }
    return buf;
}

namespace {

std::string lower(std::string_view t) {
    std::string r(t);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trim(std::string_view t) {
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.remove_prefix(1);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.remove_suffix(1);
    return t;
}

// First number in the text; also returns the rest after it.
std::optional<double> leadingNumber(std::string_view t, std::string_view* rest = nullptr) {
    const std::string str(t);
    const char* p = str.c_str();
    while (*p && !(std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.')) ++p;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return std::nullopt;
    if (rest) *rest = t.substr(static_cast<size_t>(end - str.c_str()));
    return v;
}

}  // namespace

std::optional<double> parsePlain(const ParamSpec& s, std::string_view text) {
    text = trim(text);
    const std::string t = lower(text);
    if (s.type == ParamType::Toggle) {
        if (t == "on" || t == "1" || t == "true" || t == "yes") return 1.0;
        if (t == "off" || t == "0" || t == "false" || t == "no") return 0.0;
        return std::nullopt;
    }
    if (s.type == ParamType::Choice) {
        for (size_t i = 0; i < s.choices.size(); ++i)
            if (lower(s.choices[i]) == t) return static_cast<double>(i);
        if (auto v = leadingNumber(t); v && t.find_first_not_of("0123456789. ") == std::string::npos)
            return clampPlain(s, *v);
        return std::nullopt;
    }
    if (s.unit == Unit::OutputLevelDb && (t.find("inf") != std::string::npos)) return kOutputSilenceDb;
    if (s.unit == Unit::Pan && (t == "c" || t == "center" || t == "centre")) return 0.0;
    std::string_view rest;
    auto v = leadingNumber(t, &rest);
    if (!v) return std::nullopt;
    double x = *v;
    const std::string r = lower(trim(rest));
    switch (s.unit) {
        case Unit::Log2Hz:
            if (!r.empty() && r[0] == 'k') x *= 1000.0;
            x = std::log2(std::max(x, 1e-3));
            break;
        case Unit::QPosition: x = positionFromQ(x); break;
        case Unit::Pan:
            if (!r.empty() && r[0] == 'l') x = -std::abs(x);
            else if (!r.empty() && r[0] == 'r') x = std::abs(x);
            break;
        default: break;
    }
    return clampPlain(s, x);
}

}  // namespace amq::params
