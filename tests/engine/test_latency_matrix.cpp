// Latency matrix (area 21 task "Add a latency matrix test across modes, resolutions, sample rates and
// spectral on/off"; area 02 R12, acceptance "Latency exact"; area 21 R11-R13). For every mode,
// resolution, rate and spectral state, the reported latency equals the latency function and an
// impulse through the engine (bands flat; spectral bands below threshold) comes out exactly that
// late. Linear Phase has no spectral path, so its spectral column is the same as without.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

TEST_CASE("Latency matrix: modes x resolutions x rates x spectral, measured = reported, to the sample") {
    int checked = 0;
    for (auto mode : {dsp::ProcessingMode::ZeroLatency, dsp::ProcessingMode::NaturalPhase, dsp::ProcessingMode::LinearPhase})
        for (int res = 0; res < dsp::kLatencyResolutionCount; ++res)
            for (double fs : {44100.0, 48000.0, 96000.0, 192000.0})
                for (bool spectral : {false, true}) {
                    if (mode == dsp::ProcessingMode::LinearPhase && spectral) continue;  // no spectral path
                    if (fs > 96000.0 && res == dsp::kLatencyResolutionCount - 1) continue;  // 1M-tap kernels: covered at 96 kHz
                    params::ParameterTable t;
                    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(mode), ParamSource::Preset);
                    t.set(params::globalParam(GlobalParam::Resolution), res, ParamSource::Preset);
                    if (spectral) {  // a spectral band whose threshold sits at the top: no reduction
                        t.set(params::bandParam(0, BandField::Used), 1, ParamSource::Preset);
                        t.set(params::bandParam(0, BandField::DynEnabled), 1, ParamSource::Preset);
                        t.set(params::bandParam(0, BandField::DynRange), -6.0, ParamSource::Preset);
                        t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Preset);
                        t.set(params::bandParam(0, BandField::Threshold), 0.0, ParamSource::Preset);
                        t.set(params::bandParam(0, BandField::Spectral), 1, ParamSource::Preset);
                    }
                    engine::Engine e(t);
                    REQUIRE(e.prepare(fs, 1024));
                    const uint32_t lat = e.latencySamples();
                    INFO("mode " << static_cast<int>(mode) << " resolution " << res << " fs " << fs << " spectral "
                                 << spectral << " latency " << lat);
                    CHECK(lat == dsp::latencySamples(mode, res, fs, spectral));
                    CHECK(e.activeSpectral() == spectral);
                    const size_t n = lat + 4096;
                    std::vector<float> l(n, 0.0f), r(n, 0.0f), ol(n), orr(n);
                    l[0] = r[0] = 0.5f;
                    for (size_t p = 0; p < n; p += 1024) {
                        const auto m = static_cast<uint32_t>(std::min<size_t>(1024, n - p));
                        const float* ip[2] = {l.data() + p, r.data() + p};
                        float* op[2] = {ol.data() + p, orr.data() + p};
                        e.process(ip, 2, op, 2, m);
                    }
                    size_t peak = 0;
                    for (size_t i = 0; i < n; ++i)
                        if (std::abs(ol[i]) > std::abs(ol[peak])) peak = i;
                    CHECK(peak == lat);
                    CHECK(ol[peak] == doctest::Approx(0.5).epsilon(1e-4));
                    ++checked;
                }
    MESSAGE(checked << " configurations measured");
}
