// SVF kernel variants (area 01 R18): block runs and the SSE2 linked-stereo form are bit-identical
// to the scalar per-sample tick.
#include "amq/dsp/svf.hpp"

#include <doctest/doctest.h>

#include <random>
#include <vector>

using namespace amq::dsp;

TEST_CASE("svfRun, svfRunStereo and svfTickStereo equal svfTick bit for bit") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int trial = 0; trial < 50; ++trial) {
        SvfParams p;
        p.g = std::exp(3.0 * u(rng));
        p.k = 0.05 + 2.0 * std::abs(u(rng));
        p.m0 = u(rng), p.m1 = u(rng), p.m2 = u(rng);
        const SvfCoeffs c = svfCoeffs(p);
        const uint32_t n = 1 + static_cast<uint32_t>(trial * 7 % 97);
        std::vector<double> l(n), r(n);
        for (uint32_t i = 0; i < n; ++i) l[i] = u(rng), r[i] = u(rng);
        SvfState s0{u(rng), u(rng)}, s1{u(rng), u(rng)};

        std::vector<double> el = l, er = r;  // reference: scalar ticks
        SvfState t0 = s0, t1 = s1;
        for (uint32_t i = 0; i < n; ++i) el[i] = svfTick(c, t0, el[i]), er[i] = svfTick(c, t1, er[i]);

        std::vector<double> bl = l, br = r;
        SvfState b0 = s0, b1 = s1;
        svfRunStereo(c, b0, b1, bl.data(), br.data(), n);
        CHECK(bl == el);
        CHECK(br == er);
        CHECK((b0.ic1 == t0.ic1 && b0.ic2 == t0.ic2 && b1.ic1 == t1.ic1 && b1.ic2 == t1.ic2));

        std::vector<double> ml = l;
        SvfState m0 = s0;
        svfRun(c, m0, ml.data(), n);
        CHECK(ml == el);

        std::vector<double> kl = l, kr = r;
        SvfState k0 = s0, k1 = s1;
        for (uint32_t i = 0; i < n; ++i) svfTickStereo(c, k0, k1, kl[i], kr[i]);
        CHECK(kl == el);
        CHECK(kr == er);
    }
}

TEST_CASE("svfBandEnvelopeStereo equals svfBandEnvelope per channel bit for bit") {
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int trial = 0; trial < 50; ++trial) {
        SvfParams p;
        p.g = std::exp(3.0 * u(rng));
        p.k = 0.05 + 2.0 * std::abs(u(rng));
        p.m0 = 0.0, p.m1 = p.k, p.m2 = 0.0;  // the helper band pass
        const SvfCoeffs c = svfCoeffs(p);
        const uint32_t n = 1 + static_cast<uint32_t>(trial * 7 % 97);
        std::vector<double> l(n), r(n);
        for (uint32_t i = 0; i < n; ++i) l[i] = u(rng), r[i] = u(rng);
        SvfState s0{u(rng), u(rng)}, s1{u(rng), u(rng)};
        std::vector<double> el = l, er = r;
        SvfState e0 = s0, e1 = s1;
        for (uint32_t i = 0; i < n; ++i) el[i] = svfBandEnvelope(c, e0, el[i]), er[i] = svfBandEnvelope(c, e1, er[i]);
        svfBandEnvelopeStereo(c, s0, s1, l.data(), r.data(), n);
        CHECK(l == el);
        CHECK(r == er);
        CHECK((s0.ic1 == e0.ic1 && s0.ic2 == e0.ic2 && s1.ic1 == e1.ic1 && s1.ic2 == e1.ic2));
    }
}

#include "amq/dsp/mid_side.hpp"

TEST_CASE("M/S converter: the SSE2 form equals the scalar formula bit for bit, odd lengths included") {
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (uint32_t n : {1u, 2u, 7u, 64u, 513u}) {
        std::vector<double> a(n), b(n);
        for (uint32_t i = 0; i < n; ++i) a[i] = u(rng), b[i] = u(rng);
        auto ea = a, eb = b;
        for (uint32_t i = 0; i < n; ++i) {
            const double l = ea[i], r = eb[i];
            ea[i] = 0.5 * (l + r), eb[i] = 0.5 * (l - r);
        }
        midSide(MidSideMode::Encode, a.data(), b.data(), n);
        CHECK(a == ea);
        CHECK(b == eb);
        for (uint32_t i = 0; i < n; ++i) {
            const double m = ea[i], s = eb[i];
            ea[i] = m + s, eb[i] = m - s;
        }
        midSide(MidSideMode::Decode, a.data(), b.data(), n);
        CHECK(a == ea);
        CHECK(b == eb);
    }
}
