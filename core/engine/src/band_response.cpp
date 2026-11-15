#include "amq/engine/band_response.hpp"

#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cmath>

namespace amq::engine {

using params::BandField;

dsp::BandSpec bandSpecFrom(const DesignInputs& in) noexcept {
    dsp::BandSpec spec;
    spec.shape = static_cast<dsp::Shape>(in.shape);
    spec.slope = static_cast<dsp::Slope>(in.slope);
    spec.frequencyHz = std::exp2(in.freqLog2);
    spec.gainDb = in.gainDb;
    spec.q = params::qFromPosition(in.qPos);
    return spec;
}

DesignInputs bandDesignInputs(const params::ParameterTable& t, int band, double sampleRate) noexcept {
    const double scale = t.value(params::globalParam(params::GlobalParam::GainScale)) / 100.0;
    DesignInputs in;
    in.sampleRate = sampleRate;
    in.freqLog2 = t.value(params::bandParam(band, BandField::Freq));
    in.gainDb = std::clamp(t.value(params::bandParam(band, BandField::Gain)) * scale, -30.0, 30.0);
    in.qPos = t.value(params::bandParam(band, BandField::Q));
    in.shape = static_cast<int>(t.value(params::bandParam(band, BandField::Shape)));
    in.slope = static_cast<int>(t.value(params::bandParam(band, BandField::Slope)));
    return in;
}

bool designBandCascade(const DesignInputs& in, DesignTier tier, dsp::SvfCascade& out) {
    if (tier == DesignTier::Refined) return designRefinedCascade(in, out);
    return dsp::designBandRealtime(bandSpecFrom(in), in.sampleRate, out) != dsp::RealtimeDesignResult::Unsupported;
}

bool bandMagnitudeDb(const DesignInputs& in, DesignTier tier, const double* hz, size_t n, double* outDb) {
    dsp::SvfCascade c;
    if (!designBandCascade(in, tier, c)) return false;
    for (size_t i = 0; i < n; ++i) {
        const double m = std::abs(dsp::svfCascadeResponseHz(c, hz[i], in.sampleRate));
        outDb[i] = 20.0 * std::log10(std::max(m, 1e-15));
    }
    return true;
}

}  // namespace amq::engine
