// The latency function (area 02 R9-R12): a table over every mode x resolution x rate x
// spectral combination, plus measured impulse delays through the real convolver and kernel.

#include "amq/dsp/latency.hpp"
#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/dsp/partitioned_convolver.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace amq::dsp;

namespace {

constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0};
constexpr int kFactor[] = {1, 1, 2, 2, 4, 4, 8, 8};

// Expected Linear Phase / FFT-path latency at 44.1 and 48 kHz, written out by hand from R9 and
// R11: FIR / 2 + 64.
constexpr uint32_t kLinearBase[] = {2048 + 64, 4096 + 64, 8192 + 64, 16384 + 64, 65536 + 64};

// Index of the first sample whose magnitude exceeds half the peak.
int firstPeak(const std::vector<float>& y) {
    float peak = 0.0f;
    for (float v : y) peak = std::max(peak, std::abs(v));
    for (size_t i = 0; i < y.size(); ++i)
        if (std::abs(y[i]) > 0.5f * peak) return static_cast<int>(i);
    return -1;
}

// Pushes an impulse through a convolver in host-sized blocks and returns the output.
std::vector<float> runImpulse(PartitionedConvolver& c, size_t length, size_t block = 512) {
    std::vector<float> x(length, 0.0f), y(length, 0.0f);
    x[0] = 1.0f;
    for (size_t pos = 0; pos < length; pos += block) {
        const size_t n = std::min(block, length - pos);
        c.process(x.data() + pos, y.data() + pos, n);
    }
    return y;
}

}  // namespace

TEST_CASE("Rate factor steps at 50, 100 and 200 kHz") {
    for (size_t i = 0; i < std::size(kRates); ++i) CHECK(rateFactor(kRates[i]) == kFactor[i]);
    CHECK(rateFactor(50000.0) == 1);
    CHECK(rateFactor(50001.0) == 2);
    CHECK(rateFactor(100001.0) == 4);
    CHECK(rateFactor(200001.0) == 8);
}

TEST_CASE("Latency table: every mode x resolution x rate x spectral (R9, R11)") {
    int rows = 0;
    for (size_t ri = 0; ri < std::size(kRates); ++ri) {
        const double fs = kRates[ri];
        const auto f = static_cast<uint32_t>(kFactor[ri]);
        for (int res = 0; res < kLatencyResolutionCount; ++res) {
            const uint32_t linear = kLinearBase[res] * f;  // FIR and partition both scale by f
            for (bool spectral : {false, true}) {
                CAPTURE(fs);
                CAPTURE(res);
                CAPTURE(spectral);
                CHECK(latencySamples(ProcessingMode::ZeroLatency, res, fs, spectral) == (spectral ? linear : 0u));
                CHECK(latencySamples(ProcessingMode::NaturalPhase, res, fs, spectral) ==
                      (spectral ? linear : 320u));
                CHECK(latencySamples(ProcessingMode::LinearPhase, res, fs, spectral) == linear);
                rows += 3;
            }
        }
    }
    CHECK(rows == 8 * 5 * 2 * 3);
}

TEST_CASE("Linear Phase sizes match R11 at 44.1 / 48 kHz and double per rate step") {
    const int taps[] = {4096, 8192, 16384, 32768, 131072};
    for (int res = 0; res < kLatencyResolutionCount; ++res) {
        CHECK(linearPhaseSize(res, 48000.0).firTaps == taps[res]);
        CHECK(linearPhaseSize(res, 96000.0).firTaps == 2 * taps[res]);
        CHECK(linearPhaseSize(res, 192000.0).firTaps == 4 * taps[res]);
        CHECK(linearPhaseSize(res, 384000.0).firTaps == 8 * taps[res]);
        CHECK(linearPhaseSize(res, 384000.0).partition == 8 * 64);
    }
}

TEST_CASE("Out-of-range resolutions clamp") {
    CHECK(latencySamples(ProcessingMode::LinearPhase, -3, 48000.0, false) ==
          latencySamples(ProcessingMode::LinearPhase, 0, 48000.0, false));
    CHECK(latencySamples(ProcessingMode::LinearPhase, 99, 48000.0, false) ==
          latencySamples(ProcessingMode::LinearPhase, kLatencyResolutionCount - 1, 48000.0, false));
}

TEST_CASE("Natural Phase latency does not depend on resolution or rate (area 05 R2)") {
    static_assert(kNaturalPhaseCentre == NaturalPhaseKernelBuilder::kCentre);
    static_assert(kNaturalPhasePartition == PartitionedConvolver::kDefaultPartition);
    for (double fs : kRates)
        for (int res = 0; res < kLatencyResolutionCount; ++res)
            CHECK(latencySamples(ProcessingMode::NaturalPhase, res, fs, false) == 320u);
}

TEST_CASE("Measured: an identity Natural Phase kernel delays an impulse by the reported latency") {
    NaturalPhaseKernelBuilder b;
    std::vector<float> h(NaturalPhaseKernelBuilder::kTaps);
    b.identity(h.data());
    PartitionedConvolver c;
    REQUIRE(c.prepare(kNaturalPhasePartition, NaturalPhaseKernelBuilder::kTaps));
    REQUIRE(c.setKernel(h.data(), NaturalPhaseKernelBuilder::kTaps));
    for (size_t block : {size_t{1}, size_t{37}, size_t{64}, size_t{512}}) {
        c.reset();
        CAPTURE(block);
        const auto y = runImpulse(c, 2048, block);
        CHECK(firstPeak(y) == static_cast<int>(latencySamples(ProcessingMode::NaturalPhase, 1, 48000.0, false)));
    }
}

TEST_CASE("Measured: a centred Linear Phase delta kernel delays an impulse by the reported latency") {
    // A delta at tap FIR/2 of a full-length FIR is the delay of any linear-phase kernel of that
    // length. The two highest rate steps stop at High to keep the test fast.
    for (size_t ri = 0; ri < std::size(kRates); ri += 2) {
        const double fs = kRates[ri];
        const int maxRes = kFactor[ri] >= 4 ? 2 : kLatencyResolutionCount - 1;
        for (int res = 0; res <= maxRes; ++res) {
            const LinearPhaseSize s = linearPhaseSize(res, fs);
            std::vector<float> h(static_cast<size_t>(s.firTaps), 0.0f);
            h[static_cast<size_t>(s.firTaps / 2)] = 1.0f;
            PartitionedConvolver c;
            REQUIRE(c.prepare(s.partition, s.firTaps, 0));
            REQUIRE(c.setKernel(h.data(), s.firTaps));
            const uint32_t reported = latencySamples(ProcessingMode::LinearPhase, res, fs, false);
            const auto y = runImpulse(c, reported + 4 * static_cast<size_t>(s.partition));
            CAPTURE(fs);
            CAPTURE(res);
            CHECK(firstPeak(y) == static_cast<int>(reported));
        }
    }
}

TEST_CASE("Latency is constexpr and has no placement input (R10)") {
    static_assert(latencySamples(ProcessingMode::ZeroLatency, 1, 48000.0, false) == 0);
    static_assert(latencySamples(ProcessingMode::NaturalPhase, 1, 48000.0, false) == 320);
    static_assert(latencySamples(ProcessingMode::LinearPhase, 1, 48000.0, false) == 4096 + 64);
    static_assert(latencySamples(ProcessingMode::LinearPhase, 4, 384000.0, false) == 524288 + 512);
}
