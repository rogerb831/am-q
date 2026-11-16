#pragma once
// Am-Q parameter specs (docs/params/parameter-table.md; plan areas 09, 10, 11, 21).
//
// One flat table: 24 band blocks with a fixed field order, then globals. Every parameter is
// stored in its perceptual "plain" unit (log2 Hz, dB, Q position n, 0-100 ...), in which host
// automation is linear (area 09 R3). Output Level is the one exception: it is stored in dB,
// and its host value is the position on a curve that fades to silence below -36 dB (area 11 R7).
//
// Host ids are stable hashes of the string id (area 21), so table layout and order never
// affect automation or saved state. String ids and host ids are frozen at release.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amq::params {

using ParamIndex = uint32_t;

inline constexpr int kBandCount = 24;

enum class ParamType : uint8_t { Continuous, Toggle, Choice };

// Display and mapping unit of the stored plain value.
enum class Unit : uint8_t {
    Log2Hz,         // stored log2(Hz); shown in Hz / kHz
    Db,             // dB
    QPosition,      // stored n in 0..1; shown as Q = 0.025 * 1600^n
    Percent,        // 0..200 %, Gain Scale
    Knob,           // 0..100 dimensionless (Attack, Release, Density)
    Pan,            // -100 (left / mid) .. +100 (right / side)
    OutputLevelDb,  // dB with a silence floor; host value is the curve position 0..1
    DbPerOctave,    // analyzer tilt
    None,           // toggles and choices
};

enum class ParamSource : uint8_t { Gui, Host, Preset, EqMatch, Undo };

struct ParamSpec {
    std::string id;     // stable string id, e.g. "band03.freq"; frozen at release
    std::string name;   // host-facing name, e.g. "Band 3 Frequency"
    std::string group;  // host module, e.g. "Band 3", "Output", "Analyzer"
    ParamType type = ParamType::Continuous;
    Unit unit = Unit::None;
    double min = 0.0, max = 1.0, def = 0.0;  // plain units
    std::vector<std::string> choices;        // Choice only; stored value = index
    bool automatable = true;
    bool isBypass = false;
    bool provisional = false;  // range or default not yet fixed by its owner (see the spec doc)
    uint32_t hostId = 0;       // stableHostId(id)
};

// FNV-1a 32-bit hash of the string id, masked to 31 bits (VST3 reserves ids >= 2^31).
[[nodiscard]] constexpr uint32_t stableHostId(std::string_view id) noexcept {
    uint32_t h = 2166136261u;
    for (char c : id) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h & 0x7FFFFFFFu;
}

// Band fields in their fixed order inside each band block.
enum class BandField : uint8_t {
    Used, Enabled, Freq, Gain, Q, Shape, Slope, Placement,
    DynRange, DynEnabled, DynAuto, Threshold, Attack, Release,
    ScExternal, ScMode, ScLow, ScHigh, ScListen,
    Spectral, SpectralDensity, SpectralTilt, Solo,
    Count
};
inline constexpr int kBandFieldCount = static_cast<int>(BandField::Count);

enum class GlobalParam : uint8_t {
    Mode, Resolution, GainScale, OutputLevel, OutputPan, PanMode, Invert, Bypass, AutoGain, SoloGain,
    AnalyzerShowPre, AnalyzerShowPost, AnalyzerShowExternal, AnalyzerRange, AnalyzerResolution,
    AnalyzerSpeed, AnalyzerTilt, AnalyzerFreeze, AnalyzerCollisions, AnalyzerGrab, DisplayRange,
    Count
};
inline constexpr int kGlobalCount = static_cast<int>(GlobalParam::Count);

inline constexpr ParamIndex kParamCount = kBandCount * kBandFieldCount + kGlobalCount;

[[nodiscard]] constexpr ParamIndex bandParam(int band, BandField f) noexcept {
    return static_cast<ParamIndex>(band * kBandFieldCount + static_cast<int>(f));
}
[[nodiscard]] constexpr ParamIndex globalParam(GlobalParam g) noexcept {
    return static_cast<ParamIndex>(kBandCount * kBandFieldCount + static_cast<int>(g));
}

// Ids reserved for features not in v1 (area 21 R3) and for the reserved Speakers band field.
// They are never exposed and never reused for anything else.
[[nodiscard]] const std::vector<std::string>& reservedIds();

// All specs, in table order. Built once; immutable afterwards (safe on any thread).
[[nodiscard]] const std::vector<ParamSpec>& parameterSpecs();

// Bounds-checked lookups (area 21 R1): an unknown id or out-of-range index gives nullopt.
[[nodiscard]] const ParamSpec* specAt(ParamIndex index) noexcept;
[[nodiscard]] std::optional<ParamIndex> findById(std::string_view id) noexcept;
[[nodiscard]] std::optional<ParamIndex> findByHostId(uint32_t hostId) noexcept;

// Clamp to range; Toggle and Choice round to a whole index (area 09 R1, R2).
[[nodiscard]] double clampPlain(const ParamSpec& s, double plain) noexcept;

// Host value domain: plain for every unit except OutputLevelDb (curve position 0..1).
[[nodiscard]] double hostMin(const ParamSpec& s) noexcept;
[[nodiscard]] double hostMax(const ParamSpec& s) noexcept;
[[nodiscard]] double toHost(const ParamSpec& s, double plain) noexcept;
[[nodiscard]] double fromHost(const ParamSpec& s, double hostValue) noexcept;

// 0..1 normalised value, linear in the host domain (area 09 R3).
[[nodiscard]] double toNormalised(const ParamSpec& s, double plain) noexcept;
[[nodiscard]] double fromNormalised(const ParamSpec& s, double normalised) noexcept;

// Display text and parsing (area 10 R7). parse returns nullopt for unreadable text.
// Some text depends on other parameters: Output Pan reads "L"/"R" in L/R Pan Mode and
// "M"/"S" in M/S Pan Mode (area 04 R6). The two-argument form uses the defaults (L/R);
// ParameterTable's displayContext() gives the current one. Parsing accepts either form.
struct DisplayContext {
    bool panMidSide = false;  // Pan Mode is M/S
};
[[nodiscard]] std::string formatPlain(const ParamSpec& s, double plain, const DisplayContext& ctx);
[[nodiscard]] std::string formatPlain(const ParamSpec& s, double plain);
[[nodiscard]] std::optional<double> parsePlain(const ParamSpec& s, std::string_view text);

}  // namespace amq::params
