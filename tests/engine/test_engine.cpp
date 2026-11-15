// Processor entry tests (area 01 R3-R5/R20, area 02 R3, area 03 R10-R12, area 09 R13-R15).
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

const params::ParamIndex kLevel = params::globalParam(GlobalParam::OutputLevel);
const params::ParamIndex kInvert = params::globalParam(GlobalParam::Invert);
const params::ParamIndex kPan = params::globalParam(GlobalParam::OutputPan);

std::vector<float> noise(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& x : v) x = u(rng);
    return v;
}

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n, float v = 0.0f) : l(n, v), r(n, v) {}
};

// Processes [from, to) of a stereo signal as one piece.
void piece(engine::Engine& e, const Stereo& in, Stereo& out, size_t from, size_t to) {
    const float* ip[2] = {in.l.data() + from, in.r.data() + from};
    float* op[2] = {out.l.data() + from, out.r.data() + from};
    e.process(ip, 2, op, 2, static_cast<uint32_t>(to - from));
}

int firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

}  // namespace

TEST_CASE("Safe silence when unprepared or at an unsupported rate") {
    params::ParameterTable t;
    engine::Engine e(t);
    Stereo in(64, 0.5f), out(64, 9.0f);
    piece(e, in, out, 0, 64);
    CHECK(out.l == std::vector<float>(64, 0.0f));
    CHECK_FALSE(e.prepare(22050.0, 512));
    CHECK_FALSE(e.prepare(768000.0, 512));
    out = Stereo(64, 9.0f);
    piece(e, in, out, 0, 64);
    CHECK(out.r == std::vector<float>(64, 0.0f));
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) CHECK(e.prepare(fs, 512));
}

TEST_CASE("Unity pass-through at default settings is bit-exact") {
    params::ParameterTable t;
    engine::Engine e(t);
    e.prepare(48000.0, 512);
    Stereo in(512), out(512);
    in.l = noise(512, 1);
    in.r = noise(512, 2);
    piece(e, in, out, 0, 512);
    CHECK(out.l == in.l);
    CHECK(out.r == in.r);
}

TEST_CASE("A change written before piece k starts moving at sample k") {
    for (uint32_t k : {0u, 1u, 37u, 255u, 511u}) {
        auto run = [&](bool change) {
            params::ParameterTable t;
            engine::Engine e(t);
            e.prepare(48000.0, 512);
            Stereo in(512), out(512);
            in.l = noise(512, 3);
            in.r = noise(512, 4);
            piece(e, in, out, 0, k);
            if (change) t.set(kLevel, -6.0, ParamSource::Host);
            piece(e, in, out, k, 512);
            return out.l;
        };
        INFO("offset " << k);
        CHECK(firstDifference(run(false), run(true)) == static_cast<int>(k));
    }
}

TEST_CASE("Ramp time: small moves ramp, large moves are instant, time is in ms") {
    engine::RampPolicy p{20.0, 5.0, 18.0};
    // 90 % of a small step is covered at about 20 ms at every sample rate.
    for (double fs : {44100.0, 96000.0, 384000.0}) {
        const uint32_t n = engine::rampSamples(p, 0.1, fs);
        CHECK(0.9 * n / fs * 1000.0 == doctest::Approx(20.0 - 15.0 * 0.1 / 18.0).epsilon(1e-3));
    }
    CHECK(engine::rampSamples(p, 9.0, 48000.0) < engine::rampSamples(p, 1.0, 48000.0));  // bigger = shorter
    CHECK(engine::rampSamples(p, 18.0, 48000.0) == 0);                                   // jump = instant
    CHECK(engine::rampSamples(p, 0.0, 48000.0) == 0);
}

TEST_CASE("Output level ramps without a step and settles exactly") {
    params::ParameterTable t;
    engine::Engine e(t);
    e.prepare(48000.0, 4096);
    Stereo in(4096, 1.0f), out(4096);
    t.set(kLevel, -3.0, ParamSource::Host);  // small move: ramps
    piece(e, in, out, 0, 4096);
    const double target = std::pow(10.0, -3.0 / 20.0);
    CHECK(out.l[0] < 1.0f);
    CHECK(out.l[0] > 0.99f);  // first sample moves by one ramp step only
    double maxStep = 0;
    for (size_t i = 1; i < out.l.size(); ++i) maxStep = std::max(maxStep, std::abs(double(out.l[i]) - out.l[i - 1]));
    CHECK(maxStep < 1e-3);
    CHECK(out.l.back() == doctest::Approx(target).epsilon(1e-7));
}

TEST_CASE("Retarget mid-ramp continues from the current value") {
    params::ParameterTable t;
    engine::Engine e(t);
    e.prepare(48000.0, 2048);
    Stereo in(2048, 1.0f), out(2048);
    t.set(kLevel, -6.0, ParamSource::Host);
    piece(e, in, out, 0, 300);
    t.set(kLevel, 3.0, ParamSource::Host);
    piece(e, in, out, 300, 2048);
    CHECK(std::abs(double(out.l[300]) - out.l[299]) < 1e-3);  // no jump at the retarget
}

TEST_CASE("Prepare and snapToTable seed from the table, never from defaults") {
    params::ParameterTable t;
    t.set(kLevel, -6.0, ParamSource::Preset);
    t.set(params::bandParam(4, BandField::Gain), 12.0, ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(48000.0, 64);
    CHECK(e.bandValue(4, BandField::Gain) == 12.0);
    Stereo in(64, 1.0f), out(64);
    piece(e, in, out, 0, 64);
    CHECK(out.l[0] == doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(1e-6));
    t.set(kLevel, 0.0, ParamSource::Preset);
    e.snapToTable();  // state restore: no ramp
    piece(e, in, out, 0, 64);
    CHECK(out.l[0] == 1.0f);
}

TEST_CASE("Band ramps follow the table at piece starts") {
    params::ParameterTable t;
    engine::Engine e(t);
    e.prepare(48000.0, 48000);
    Stereo in(48000, 0.0f), out(48000);
    t.set(params::bandParam(0, BandField::Gain), 1.0, ParamSource::Host);
    piece(e, in, out, 0, 10);
    const double mid = e.bandValue(0, BandField::Gain);
    CHECK(mid > 0.0);
    CHECK(mid < 1.0);
    piece(e, in, out, 10, 48000);
    CHECK(e.bandValue(0, BandField::Gain) == 1.0);
    t.set(params::bandParam(0, BandField::Freq), std::log2(8000.0), ParamSource::Host);  // > 2 octaves: jump
    piece(e, in, out, 0, 1);
    CHECK(e.bandValue(0, BandField::Freq) == doctest::Approx(std::log2(8000.0)));
}

TEST_CASE("Invert negates both outputs, ramping through zero") {
    params::ParameterTable t;
    engine::Engine e(t);
    e.prepare(48000.0, 4096);
    Stereo in(4096), out(4096);
    in.l = noise(4096, 5);
    in.r = noise(4096, 6);
    t.set(kInvert, 1.0, ParamSource::Host);
    piece(e, in, out, 0, 4096);
    CHECK(out.l[0] != -in.l[0]);  // ramped, not stepped
    for (size_t i = 2048; i < 4096; ++i) {
        CHECK(out.l[i] == -in.l[i]);
        CHECK(out.r[i] == -in.r[i]);
    }
}

TEST_CASE("Pan is a linear balance that only attenuates") {
    params::ParameterTable t;
    t.set(kPan, 50.0, ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(48000.0, 64);
    Stereo in(64, 1.0f), out(64);
    piece(e, in, out, 0, 64);
    CHECK(out.l[10] == doctest::Approx(0.5));
    CHECK(out.r[10] == 1.0f);
    t.set(kPan, -100.0, ParamSource::Preset);
    e.snapToTable();
    piece(e, in, out, 0, 64);
    CHECK(out.l[10] == 1.0f);
    CHECK(out.r[10] == 0.0f);
}

TEST_CASE("Block-size robustness: identical output for any piece sizes") {
    const size_t n = 6000;
    auto run = [&](const std::vector<uint32_t>& sizes) {
        params::ParameterTable t;
        engine::Engine e(t);
        e.prepare(48000.0, 4096);
        Stereo in(n), out(n);
        in.l = noise(n, 7);
        in.r = noise(n, 8);
        size_t pos = 0, k = 0;
        bool changed = false;
        while (pos < n) {
            const size_t len = std::min<size_t>(sizes[k++ % sizes.size()], n - pos);
            if (!changed && pos >= 1000) {  // same absolute time in every run: split there
                t.set(kLevel, -4.0, ParamSource::Host);
                changed = true;
            }
            const size_t end = (!changed && pos + len > 1000) ? 1000 : pos + len;
            piece(e, in, out, pos, end);
            pos = end;
        }
        return out.l;
    };
    const auto ref = run({64});
    CHECK(run({1}) == ref);
    CHECK(run({2}) == ref);
    CHECK(run({3}) == ref);
    CHECK(run({4096}) == ref);
    CHECK(run({17, 1, 300, 5, 64, 999}) == ref);
}

TEST_CASE("In-place processing and mono input") {
    params::ParameterTable t;
    t.set(kLevel, -6.0, ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(48000.0, 64);
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* io[2] = {l.data(), r.data()};
    e.process(io, 2, io, 2, 64);
    CHECK(l[5] == doctest::Approx(0.501187).epsilon(1e-5));
    std::vector<float> mono(64, 1.0f), ol(64), orr(64);
    const float* mi[1] = {mono.data()};
    float* oo[2] = {ol.data(), orr.data()};
    e.process(mi, 1, oo, 2, 64);
    CHECK(ol[5] == orr[5]);
}

TEST_CASE("The engine meters input and output and publishes snapshots (areas 11, 12, 14)") {
    params::ParameterTable t;
    engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    t.set(kLevel, -6.0, ParamSource::Host);
    e.snapToTable();
    std::vector<float> l(512), r(512, 0.0f), ol(512), orr(512);
    uint64_t n = 0;
    for (int blk = 0; blk < 94; ++blk) {  // about 1 s of a 0.5-amplitude sine on the left only
        for (size_t i = 0; i < l.size(); ++i) l[i] = static_cast<float>(0.5 * std::sin(0.1 * static_cast<double>(n + i)));
        const float* in[2] = {l.data(), r.data()};
        float* out[2] = {ol.data(), orr.data()};
        e.process(in, 2, out, 2, 512);
        n += 512;
    }
    const auto* s = e.snapshots().pullLevels();
    REQUIRE(s != nullptr);
    CHECK(e.snapshots().publishedCount() == doctest::Approx(60.0).epsilon(0.05));
    CHECK(s->channels == 2);
    CHECK(s->input[0].peakDb == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(s->output[0].peakDb == doctest::Approx(-12.04).epsilon(0.01));  // after Output Level -6 dB
    CHECK(s->input[1].peakDb == analysis::kFloorDb);
    CHECK(s->output[1].peakDb == analysis::kFloorDb);
    const auto* g = e.snapshots().pullGainChange();
    REQUIRE(g != nullptr);
    CHECK(g->currentDb[0] == 0.0f);  // no dynamics yet (area 06)
}
