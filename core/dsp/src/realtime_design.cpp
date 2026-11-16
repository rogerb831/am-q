#include "amq/dsp/realtime_design.hpp"

#include <algorithm>

namespace amq::dsp {

namespace {

bool toCascade(const DigitalFilter& f, SvfCascade& out) noexcept {
    SvfCascade c;
    for (const auto& b : f.sections) {
        SvfParams s;
        if (!svfFromBiquad(b, s)) return false;
        c.sections.push_back(s);
    }
    if (c.sections.overflowed()) return false;
    out = c;
    return true;
}

}  // namespace

RealtimeDesignResult designBandRealtime(const BandSpec& spec, double sampleRate, SvfCascade& out) noexcept {
    const AnalogPrototype proto = designAnalogPrototype(spec);
    if (!proto.supported || proto.sections.overflowed()) return RealtimeDesignResult::Unsupported;
    if (proto.brickwall) {
        // Brickwall: bilinear pre-warped at f0 (BilinearCentre): the map only warps frequency, so
        // the flat passband and the equiripple stopband keep their depth, and -3 dB stays at f0.
        if (toCascade(designBilinear(proto, sampleRate, std::min(spec.frequencyHz, 0.45 * sampleRate)), out))
            return RealtimeDesignResult::Matched;
        return RealtimeDesignResult::Unsupported;
    }
    if (spec.shape != Shape::AllPass) {
        if (toCascade(designMatched(proto, sampleRate, DesignMethod::MatchedThreePoint), out))
            return RealtimeDesignResult::Matched;
    } else if (toCascade(designBilinear(proto, sampleRate), out)) {
        return RealtimeDesignResult::Matched;  // All Pass: bilinear pre-warped at f0 is its design (area 02 R15)
    }
    if (toCascade(designBilinear(proto, sampleRate, std::min(spec.frequencyHz, 0.45 * sampleRate)), out))
        return RealtimeDesignResult::BilinearFallback;
    return RealtimeDesignResult::Unsupported;
}

}  // namespace amq::dsp
