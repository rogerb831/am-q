#pragma once
// Am-Q CLAP-native spike: parameter table shared by the plugin, the editor and the tests.
// Spike only; the real table lives in core/params (Phase 1).

#include "amq/dsp/conventions.hpp"

#include <clap/clap.h>

#include <cmath>
#include <cstdio>

namespace amq::spike {

enum ParamId : clap_id {
    kFreq = 0,     // stored as log2(Hz), 10 Hz .. 30 kHz (plan "Encodings")
    kGain = 1,     // band gain, dB, -30 .. +30
    kQ = 2,        // normalised n, Q = 0.025 * 1600^n (area 10 R2)
    kOutGain = 3,  // output gain, dB, -30 .. +30 (used for the sample-accuracy test)
    kLatency = 4,  // dummy latency toggle: 0 = none, 1 = kDummyLatency samples
    kParamCount = 5,
};

inline constexpr uint32_t kDummyLatency = 64;

struct ParamDef {
    const char* name;
    double min, max, def;
    bool stepped;
};

inline const ParamDef kParams[kParamCount] = {
    {"Freq", 3.321928094887362, 14.872674880270605, 9.965784284662087, false},  // log2(10), log2(30000), log2(1000)
    {"Gain", -30.0, 30.0, 0.0, false},
    {"Q", 0.0, 1.0, 0.5, false},
    {"Output Gain", -30.0, 30.0, 0.0, false},
    {"Dummy Latency", 0.0, 1.0, 0.0, true},
};

inline double qFromNormalised(double n) { return 0.025 * std::pow(1600.0, n); }
inline double normalisedFromQ(double q) { return std::log(q / 0.025) / std::log(1600.0); }

inline void formatParam(clap_id id, double v, char* out, size_t size) {
    switch (id) {
        case kFreq: {
            const double hz = std::exp2(v);
            if (hz >= 1000.0) std::snprintf(out, size, "%.2f kHz", hz / 1000.0);
            else std::snprintf(out, size, "%.0f Hz", hz);
            break;
        }
        case kGain:
        case kOutGain: std::snprintf(out, size, "%+.1f dB", v); break;
        case kQ: std::snprintf(out, size, "Q %.2f", qFromNormalised(v)); break;
        case kLatency: std::snprintf(out, size, "%s", v >= 0.5 ? "64 samples" : "Off"); break;
        default: std::snprintf(out, size, "%g", v); break;
    }
}

}  // namespace amq::spike
