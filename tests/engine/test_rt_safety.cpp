// Real-time safety and fuzz (area 01 R19 and its acceptance rows "Real-time safety" and "Fuzz").
//
// Allocation detector: this test binary replaces the global allocation functions. While the
// calling thread is marked as the audio thread, every allocation is counted. Engine::process must
// make none, in every mode and feature path. (Locks: the engine uses none on the audio thread by
// construction, sequence slots and atomics only; there is no portable lock detector here.)
//
// Fuzz: random parameters, sample rates and block sizes; the output must stay finite. The short
// run is part of the suite; set AMQ_FUZZ_SECONDS for the long run (the acceptance asks 1 hour).
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "alloc_counter.hpp"

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

using AudioThreadScope = amq_test::CountAllocsScope;

void randomBand(params::ParameterTable& t, int b, std::mt19937& rng, ParamSource src) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int f = 0; f < params::kBandFieldCount; ++f) {
        const auto idx = params::bandParam(b, static_cast<BandField>(f));
        const auto& s = *params::specAt(idx);
        t.set(idx, params::fromNormalised(s, u(rng)), src);
    }
}

void randomGlobals(params::ParameterTable& t, std::mt19937& rng, ParamSource src, bool modeToo) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (auto g : {GlobalParam::GainScale, GlobalParam::OutputLevel, GlobalParam::OutputPan, GlobalParam::PanMode,
                   GlobalParam::Invert, GlobalParam::Bypass, GlobalParam::SoloGain, GlobalParam::Resolution}) {
        const auto idx = params::globalParam(g);
        t.set(idx, params::fromNormalised(*params::specAt(idx), u(rng)), src);
    }
    if (modeToo) t.set(params::globalParam(GlobalParam::Mode), u(rng) < 0.5 ? 0 : 1, src);
}

}  // namespace

TEST_CASE("Real-time safety: Engine::process allocates nothing in any mode or feature path") {
    {  // the detector sees allocations on a marked thread
        amq_test::allocs = 0;
        AudioThreadScope audio;
        auto* p = new std::vector<int>(4);
        delete p;
        CHECK(amq_test::allocs.load() == 2);
    }
    for (auto mode : {dsp::ProcessingMode::ZeroLatency, dsp::ProcessingMode::NaturalPhase}) {
        params::ParameterTable t;
        std::mt19937 rng(11 + static_cast<int>(mode));
        for (int b = 0; b < params::kBandCount; ++b) randomBand(t, b, rng, ParamSource::Preset);
        t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(mode), ParamSource::Preset);
        engine::Engine e(t);
        REQUIRE(e.prepare(48000.0, 512));
        std::vector<float> l(512), r(512), sl(512), sr(512), ol(512), or_(512);
        std::uniform_real_distribution<float> s(-0.5f, 0.5f);
        std::uniform_int_distribution<int> blk(1, 512);
        amq_test::allocs = 0;
        for (int k = 0; k < 3000; ++k) {
            // Edits between blocks, as the host's events would make them: band fields (shape and
            // slope changes, solo, enable), globals (pan mode, bypass, solo gain).
            if (k % 7 == 0) randomBand(t, k % params::kBandCount, rng, ParamSource::Host);
            if (k % 97 == 0) randomGlobals(t, rng, ParamSource::Host, false);
            for (auto* v : {&l, &r, &sl, &sr})
                for (auto& x : *v) x = s(rng);
            const float* ip[2] = {l.data(), r.data()};
            const float* sp[2] = {sl.data(), sr.data()};
            float* op[2] = {ol.data(), or_.data()};
            const auto n = static_cast<uint32_t>(blk(rng));
            AudioThreadScope audio;
            e.process(ip, 2, op, 2, n, sp, 2);
        }
        INFO("mode " << static_cast<int>(mode));
        CHECK(amq_test::allocs.load() == 0);
    }
}

TEST_CASE("Fuzz: random parameters, rates and block sizes keep the output finite") {
    double budget = 3.0;
#if defined(_MSC_VER)
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "AMQ_FUZZ_SECONDS") == 0 && env) {
        budget = std::atof(env);
        std::free(env);
    }
#else
    if (const char* env = std::getenv("AMQ_FUZZ_SECONDS")) budget = std::atof(env);
#endif
    const auto t0 = std::chrono::steady_clock::now();
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    uint64_t runs = 0, samples = 0;
    bool finite = true;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < budget && finite) {
        params::ParameterTable t;
        for (int b = 0; b < params::kBandCount; ++b)
            if (u(rng) < 0.6) randomBand(t, b, rng, ParamSource::Preset);
        randomGlobals(t, rng, ParamSource::Preset, true);
        const double fs = rates[static_cast<size_t>(u(rng) * 8) % 8];
        const uint32_t maxFrames = 1 + static_cast<uint32_t>(u(rng) * 2048);
        const uint32_t ch = u(rng) < 0.2 ? 1 : 2;
        engine::Engine e(t);
        REQUIRE(e.prepare(fs, maxFrames));
        std::vector<float> l(maxFrames), r(maxFrames), ol(maxFrames), or_(maxFrames);
        for (int k = 0; k < 200 && finite; ++k) {
            if (u(rng) < 0.3) randomBand(t, static_cast<int>(u(rng) * params::kBandCount) % params::kBandCount, rng, ParamSource::Host);
            if (u(rng) < 0.02) randomGlobals(t, rng, ParamSource::Host, false);
            const double kind = u(rng);  // noise, silence, full-scale square, tiny values
            for (uint32_t i = 0; i < maxFrames; ++i) {
                float v = static_cast<float>(u(rng) * 2.0 - 1.0);
                if (kind < 0.1) v = 0.0f;
                else if (kind < 0.2) v = (i / 16) % 2 ? 1.0f : -1.0f;
                else if (kind < 0.3) v *= 1e-30f;
                l[i] = v, r[i] = -v * 0.5f;
            }
            const uint32_t n = 1 + static_cast<uint32_t>(u(rng) * maxFrames) % maxFrames;
            const float* ip[2] = {l.data(), r.data()};
            float* op[2] = {ol.data(), or_.data()};
            e.process(ip, ch, op, ch, n);
            for (uint32_t i = 0; i < n; ++i) finite &= std::isfinite(ol[i]) && (ch < 2 || std::isfinite(or_[i]));
            samples += n;
        }
        ++runs;
    }
    INFO(runs << " configurations, " << samples << " samples");
    CHECK(finite);
    CHECK(runs > 0);
}
