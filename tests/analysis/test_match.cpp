// EQ Match captures and difference curve (area 17 R4, R5, R8, R13; area 18 R1-R7; area 20 R1, R3-R7, R10).
#include "amq/analysis/audio_file.hpp"
#include "amq/analysis/match_curve.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <cmath>
#include <random>
#include <vector>

using namespace amq::analysis;

namespace {

constexpr double kFs = 48000.0;
constexpr int kN = 16384;  // the analyzer's highest size at 48 kHz
constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(size_t n, double hz, double amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * double(i) / kFs));
    return x;
}

std::vector<float> noise(size_t n, double amp, uint32_t seed, double lowpassCoeff = 0.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, amp);
    std::vector<float> x(n);
    double y = 0.0;
    for (auto& v : x) {
        y = lowpassCoeff * y + (1.0 - lowpassCoeff) * g(rng);
        v = static_cast<float>(lowpassCoeff > 0.0 ? y : g(rng));
    }
    return x;
}

void feed(MatchCapture& c, const std::vector<float>& l, const std::vector<float>& r) {
    const float* ch[2] = {l.data(), r.data()};
    c.analyseSignal(ch, 2, l.size());
}

}  // namespace

TEST_CASE("Match grid: 1/48 octave from 20 Hz, 480 points to 20 kHz") {
    const auto& g = matchGrid();
    CHECK(g[0] == 20.0);
    CHECK(std::log2(g[1] / g[0]) == doctest::Approx(1.0 / 48.0));
    CHECK(g.back() == doctest::Approx(20000.0).epsilon(0.02));
}

TEST_CASE("Capture: a tone peaks at its grid point; silence is gated; ready after 2 s of counted audio") {
    MatchCapture c;
    c.prepare(kFs, kN);
    const auto x = tone(static_cast<size_t>(3 * kFs), 1000.0, 0.5);
    std::vector<float> silence(static_cast<size_t>(kFs), 0.0f);
    feed(c, silence, silence);
    CHECK(c.countedFrames() == 0);
    CHECK(c.skippedFrames() > 0);  // gated (area 18 R2)
    CHECK_FALSE(c.ready());
    feed(c, x, x);
    CHECK(c.countedSeconds() >= 2.0);
    CHECK(c.ready());
    std::array<double, kMatchGridPoints> db{};
    REQUIRE(c.meanDb(db));
    const auto at = std::max_element(db.begin(), db.end()) - db.begin();
    CHECK(std::abs(std::log2(matchGrid()[static_cast<size_t>(at)] / 1000.0)) <= 1.0 / 48.0);
    // Channel merge by mean power (area 17 R5): a hard-left tone reads 3 dB below a centred one.
    MatchCapture left;
    left.prepare(kFs, kN);
    std::vector<float> zero(x.size(), 0.0f);
    feed(left, x, zero);
    std::array<double, kMatchGridPoints> dl{};
    REQUIRE(left.meanDb(dl));
    CHECK(dl[static_cast<size_t>(at)] == doctest::Approx(db[static_cast<size_t>(at)] - 10.0 * std::log10(2.0)).epsilon(1e-3));
}

TEST_CASE("Capture: last-N-seconds forgets old audio; whole capture keeps it; reset clears") {
    MatchCapture c;
    c.prepare(kFs, kN);
    feed(c, tone(static_cast<size_t>(4 * kFs), 200.0, 0.5), tone(static_cast<size_t>(4 * kFs), 200.0, 0.5));
    feed(c, tone(static_cast<size_t>(4 * kFs), 5000.0, 0.5), tone(static_cast<size_t>(4 * kFs), 5000.0, 0.5));
    auto level = [&](double hz) {
        std::array<double, kMatchGridPoints> db{};
        c.meanDb(db);
        const auto& g = matchGrid();
        size_t k = 0;
        for (size_t i = 0; i < g.size(); ++i)
            if (std::abs(std::log2(g[i] / hz)) < std::abs(std::log2(g[k] / hz))) k = i;
        return db[k];
    };
    const double whole200 = level(200.0);
    c.setWindow(MatchCapture::Window::LastSeconds, 2.0);
    CHECK(level(200.0) < whole200 - 30.0);  // the 200 Hz tone ended more than 2 s ago
    CHECK(level(5000.0) > level(200.0) + 30.0);
    c.setWindow(MatchCapture::Window::Whole, 10.0);
    CHECK(level(200.0) == doctest::Approx(whole200));
    c.requestReset();
    feed(c, std::vector<float>(kN * 2, 0.0f), std::vector<float>(kN * 2, 0.0f));  // the next frame applies it
    CHECK(c.countedFrames() == 0);
}

TEST_CASE("Capture: an hour of frames accumulates without drift (area 18 R5)") {
    MatchCapture c;
    c.prepare(kFs, kN);
    c.setRecording(true);
    std::vector<float> p(kN / 2 + 1);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.5f, 2.0f);
    for (auto& v : p) v = 1e3f * u(rng);
    const int frames = static_cast<int>(3600.0 * kFs / (kN / 4));
    c.addFrame(p.data(), kN / 2 + 1);
    std::array<double, kMatchGridPoints> one{}, many{};
    REQUIRE(c.meanDb(one));
    for (int i = 1; i < frames; ++i) c.addFrame(p.data(), kN / 2 + 1);
    REQUIRE(c.meanDb(many));
    double worst = 0.0;
    for (int i = 0; i < kMatchGridPoints; ++i) worst = std::max(worst, std::abs(many[static_cast<size_t>(i)] - one[static_cast<size_t>(i)]));
    CHECK(worst < 0.01);
    CHECK(c.countedSeconds() == doctest::Approx(3600.0).epsilon(1e-3));
}

TEST_CASE("Difference curve: reference minus input, level aligned by loudness, smoothed, weighted") {
    const size_t n = static_cast<size_t>(6 * kFs);
    const auto a = noise(n, 0.05, 1), b = noise(n, 0.05, 2);
    MatchCapture in, ref;
    in.prepare(kFs, kN), ref.prepare(kFs, kN);
    MatchCurve curve;
    CHECK_FALSE(computeMatchCurve(in, ref, {}, curve));  // not ready: no curve, never a flat one (R7)
    feed(in, a, a);
    std::vector<float> loud(b.size());
    for (size_t i = 0; i < b.size(); ++i) loud[i] = 2.0f * b[i];  // +6 dB broadband
    feed(ref, loud, loud);
    REQUIRE(computeMatchCurve(in, ref, {}, curve));
    CHECK(curve.levelOffsetDb == doctest::Approx(6.02).epsilon(0.05));
    double meanAligned = 0.0;
    int k = 0;
    for (int i = 50; i < 400; ++i) meanAligned += curve.target[static_cast<size_t>(i)], ++k;
    CHECK(std::abs(meanAligned / k) < 0.5);  // aligned: about 0 dB
    MatchCurveSettings raw;
    raw.levelAlign = false;
    REQUIRE(computeMatchCurve(in, ref, raw, curve));
    CHECK(curve.target[200] == doctest::Approx(6.0).epsilon(0.15));
    for (double w : curve.weight) {
        CHECK(w >= 0.0);
        CHECK(w <= 1.0);
    }
    CHECK(curve.weight.back() == 0.0);  // above the usable limit (0.45 fs at 48 kHz is 21.6 kHz: 20 kHz grid end may count)
}

TEST_CASE("Fractional-octave smoothing: width in octaves, edges shrink, a constant stays constant") {
    std::array<double, kMatchGridPoints> step{}, out{};
    for (int i = 0; i < kMatchGridPoints; ++i) step[static_cast<size_t>(i)] = i < 240 ? 0.0 : 12.0;
    smoothFractionalOctave(step, 1.0 / 6.0, out);
    CHECK(out[0] == 0.0);  // the edge window shrinks to the point itself
    CHECK(out.back() == 12.0);
    CHECK(out[240] > 0.0);
    CHECK(out[240] < 12.0);
    CHECK(out[235] == 0.0);  // 1/6 octave = 8 points: half-width 4 (235 +- 4 stays below 240)
    CHECK(out[244] == 12.0);
    std::array<double, kMatchGridPoints> flat{};
    flat.fill(3.5);
    smoothFractionalOctave(flat, 1.0 / 3.0, out);
    for (double v : out) CHECK(v == doctest::Approx(3.5));
}

namespace {
std::vector<unsigned char> wavBytes(int channels, int rate, int bits, bool isFloat, const std::vector<double>& frames,
                                    bool extensible = false) {
    auto put32 = [](std::vector<unsigned char>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    };
    auto put16 = [](std::vector<unsigned char>& b, uint16_t v) {
        b.push_back(static_cast<unsigned char>(v));
        b.push_back(static_cast<unsigned char>(v >> 8));
    };
    std::vector<unsigned char> data;
    for (double v : frames)
        for (int c = 0; c < channels; ++c) {
            const double x = c == 0 ? v : -v;
            if (isFloat) {
                float f = static_cast<float>(x);
                unsigned char t[4];
                std::memcpy(t, &f, 4);
                data.insert(data.end(), t, t + 4);
            } else if (bits == 16) {
                put16(data, static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 32767))));
            } else {
                const auto i = static_cast<int32_t>(std::lround(x * 8388607));
                data.push_back(static_cast<unsigned char>(i)), data.push_back(static_cast<unsigned char>(i >> 8)),
                    data.push_back(static_cast<unsigned char>(i >> 16));
            }
        }
    std::vector<unsigned char> b = {'R', 'I', 'F', 'F'};
    put32(b, 0);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, extensible ? 40 : 16);
    put16(b, extensible ? 0xFFFE : (isFloat ? 3 : 1));
    put16(b, static_cast<uint16_t>(channels));
    put32(b, static_cast<uint32_t>(rate));
    put32(b, static_cast<uint32_t>(rate * channels * bits / 8));
    put16(b, static_cast<uint16_t>(channels * bits / 8));
    put16(b, static_cast<uint16_t>(bits));
    if (extensible) {
        put16(b, 22), put16(b, static_cast<uint16_t>(bits)), put32(b, 3);
        put16(b, isFloat ? 3 : 1);  // the sub-format GUID's first two bytes
        for (int i = 0; i < 14; ++i) b.push_back(0);
    }
    b.insert(b.end(), {'L', 'I', 'S', 'T'});  // an unknown chunk to skip
    put32(b, 3);
    b.insert(b.end(), {1, 2, 3, 0});           // odd size plus pad byte
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, static_cast<uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}
}  // namespace

TEST_CASE("WAV reader: PCM 16 and 24-bit, float, extensible; garbage refused with a message") {
    const std::vector<double> v = {0.0, 0.5, -0.25, 0.999};
    for (auto [bits, isFloat, ext] : {std::tuple{16, false, false}, std::tuple{24, false, false}, std::tuple{32, true, false},
                                      std::tuple{24, false, true}}) {
        AudioFile f;
        std::string err;
        REQUIRE(parseWav(wavBytes(2, 44100, bits, isFloat, v, ext), f, &err));
        CHECK(f.sampleRate == 44100.0);
        REQUIRE(f.channels.size() == 2);
        REQUIRE(f.frames() == v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            CHECK(f.channels[0][i] == doctest::Approx(v[i]).epsilon(1e-4));
            CHECK(f.channels[1][i] == doctest::Approx(-v[i]).epsilon(1e-4));
        }
    }
    AudioFile f;
    std::string err;
    CHECK_FALSE(parseWav({'n', 'o', 'p', 'e'}, f, &err));
    CHECK(err == "Not a WAV file.");
}

TEST_CASE("Resampling 44.1 to 48 kHz keeps a tone's level and frequency") {
    AudioFile f;
    f.sampleRate = 44100.0;
    f.channels.assign(1, std::vector<float>(44100));
    for (size_t i = 0; i < 44100; ++i) f.channels[0][i] = static_cast<float>(0.5 * std::sin(2 * kPi * 3000.0 * i / 44100.0));
    resample(f, 48000.0);
    CHECK(f.sampleRate == 48000.0);
    CHECK(f.frames() == 48000);
    // Amplitude in the middle (away from the edges), against the ideal 3 kHz sine at 48 kHz.
    double worst = 0.0;
    for (size_t i = 10000; i < 38000; ++i)
        worst = std::max(worst, std::abs(f.channels[0][i] - 0.5 * std::sin(2 * kPi * 3000.0 * i / 48000.0)));
    CHECK(worst < 0.5 * (std::pow(10.0, 0.05 / 20.0) - 1.0) + 1e-3);  // within 0.05 dB
}

// Report (area 18 task "prototype magnitude versus power averaging and peak versus broadband
// gating on test material"). Frames are made in the spectral domain: a windowed noise frame's bin
// powers are the model spectrum times independent exponential draws (chi-square, 2 degrees of
// freedom), so every variant sees the same frames. The EQ curve is reference minus input in dB per
// 1/3 octave, 25 Hz to 16 kHz. Run with
//   amq_analysis_tests -tc="EQ Match averaging and gating*" --no-skip
TEST_CASE("EQ Match averaging and gating prototype (report)" * doctest::skip()) {
    constexpr double fs = 48000.0;
    constexpr int bins = 2049;  // 4096-point frames
    constexpr double binHz = fs / 4096.0;
    std::vector<double> centres;
    for (double f = 25.0; f <= 16000.0 * 1.01; f *= std::exp2(1.0 / 3.0)) centres.push_back(f);
    const size_t nb = centres.size();
    std::vector<int> band(bins, -1);
    for (int k = 0; k < bins; ++k)
        for (size_t b = 0; b < nb; ++b)
            if (k * binHz >= centres[b] * std::exp2(-1.0 / 6.0) && k * binHz < centres[b] * std::exp2(1.0 / 6.0))
                band[static_cast<size_t>(k)] = static_cast<int>(b);
    using Spec = std::vector<double>;  // model power per bin
    auto shelf = [&](double cornerHz, double db) {  // a smooth high shelf, in power
        Spec s(bins);
        for (int k = 0; k < bins; ++k) {
            const double x = std::log2(std::max(k * binHz, 1.0) / cornerHz);
            s[static_cast<size_t>(k)] = std::pow(10.0, db / 10.0 * (0.5 + 0.5 * std::tanh(2.0 * x)));
        }
        return s;
    };
    auto tilt = [&](double dbPerOct) {
        Spec s(bins);
        for (int k = 0; k < bins; ++k)
            s[static_cast<size_t>(k)] = std::pow(10.0, dbPerOct / 10.0 * std::log2(std::max(k * binHz, 20.0) / 1000.0));
        return s;
    };
    auto scaled = [](Spec s, double db) {
        for (auto& v : s) v *= std::pow(10.0, db / 10.0);
        return s;
    };
    auto times = [](Spec a, const Spec& b) {
        for (size_t i = 0; i < a.size(); ++i) a[i] *= b[i];
        return a;
    };
    struct Capture {  // per-bin sums of power and of magnitude over the counted frames
        Spec pow, mag;
        double counted = 0;
        explicit Capture(int n) : pow(static_cast<size_t>(n), 0.0), mag(static_cast<size_t>(n), 0.0) {}
    };
    enum class Gate { None, Broadband, Peak };
    auto add = [&](Capture& c, const Spec& model, std::mt19937& rng, Gate gate) {
        std::exponential_distribution<double> ex(1.0);
        Spec frame(bins);
        double sum = 0.0, peak = 0.0;
        int inRange = 0;
        for (int k = 0; k < bins; ++k) {
            const double p = model[static_cast<size_t>(k)] * ex(rng);
            frame[static_cast<size_t>(k)] = p;
            if (k * binHz >= 20.0 && k * binHz <= 20000.0) sum += p, ++inRange, peak = std::max(peak, p);
        }
        if (gate == Gate::Broadband && 10.0 * std::log10(sum / inRange + 1e-300) < -90.0) return;
        if (gate == Gate::Peak && 10.0 * std::log10(peak + 1e-300) < -90.0) return;
        for (int k = 0; k < bins; ++k) {
            c.pow[static_cast<size_t>(k)] += frame[static_cast<size_t>(k)];
            c.mag[static_cast<size_t>(k)] += std::sqrt(frame[static_cast<size_t>(k)]);
        }
        c.counted += 1;
    };
    // Per band: dB of the mean power (power averaging) or of the squared mean magnitude.
    auto bandsDb = [&](const Capture& c, bool magnitude) {
        std::vector<double> acc(nb, 0.0), cnt(nb, 0.0);
        for (int k = 0; k < bins; ++k) {
            const int b = band[static_cast<size_t>(k)];
            if (b < 0) continue;
            const double m = magnitude ? std::pow(c.mag[static_cast<size_t>(k)] / c.counted, 2.0)
                                       : c.pow[static_cast<size_t>(k)] / c.counted;
            acc[static_cast<size_t>(b)] += m, cnt[static_cast<size_t>(b)] += 1;
        }
        std::vector<double> db(nb);
        for (size_t b = 0; b < nb; ++b) db[b] = 10.0 * std::log10(acc[b] / std::max(cnt[b], 1.0));
        return db;
    };
    auto modelBandsDb = [&](const std::vector<Spec>& sections) {  // equal time in each section
        std::vector<double> acc(nb, 0.0), cnt(nb, 0.0);
        for (int k = 0; k < bins; ++k) {
            const int b = band[static_cast<size_t>(k)];
            if (b < 0) continue;
            double m = 0.0;
            for (const auto& s : sections) m += s[static_cast<size_t>(k)] / sections.size();
            acc[static_cast<size_t>(b)] += m, cnt[static_cast<size_t>(b)] += 1;
        }
        std::vector<double> db(nb);
        for (size_t b = 0; b < nb; ++b) db[b] = 10.0 * std::log10(acc[b] / std::max(cnt[b], 1.0));
        return db;
    };
    auto maxAbsDiff = [&](const std::vector<double>& a, const std::vector<double>& b) {
        double m = 0.0;
        for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
        return m;
    };
    auto minus = [](std::vector<double> a, const std::vector<double>& b) {
        for (size_t i = 0; i < a.size(); ++i) a[i] -= b[i];
        return a;
    };

    // A. Level-dependent material, 60 s (700 frames): the input alternates loud, brighter sections
    // with quiet, darker ones; the reference is a different take of the same pattern, with a +6 dB
    // shelf above 4 kHz in its loud sections only. Truth: the ratio of the long-term power spectra.
    for (double quietDb : {-24.0, -8.0}) {
        const Spec inLoud = scaled(tilt(-1.5), -20.0), inQuiet = scaled(tilt(-4.5), -20.0 + quietDb);
        const Spec refLoud = times(inLoud, shelf(4000.0, 6.0)), refQuiet = inQuiet;
        std::mt19937 rng(3);
        Capture in(bins), ref(bins);
        for (int f = 0; f < 700; ++f) {
            const bool loud = (f / 12) % 2 == 0;  // about 0.5 s sections
            add(in, loud ? inLoud : inQuiet, rng, Gate::Broadband);
            add(ref, loud ? refLoud : refQuiet, rng, Gate::Broadband);
        }
        const auto truth = minus(modelBandsDb({refLoud, refQuiet}), modelBandsDb({inLoud, inQuiet}));
        const auto powEq = minus(bandsDb(ref, false), bandsDb(in, false));
        const auto magEq = minus(bandsDb(ref, true), bandsDb(in, true));
        std::printf("A. Level-dependent material, quiet sections %+.0f dB (60 s): max |EQ curve - long-term power truth|\n",
                    quietDb);
        std::printf("   power averaging %.2f dB, magnitude averaging %.2f dB (at 16 kHz: truth %+.2f, power %+.2f, magnitude %+.2f)\n",
                    maxAbsDiff(powEq, truth), maxAbsDiff(magEq, truth), truth.back(), powEq.back(), magEq.back());
    }
    // B. Stationary material, short captures (3 s, 35 frames), 20 seeds: spread of the EQ curve.
    {
        const Spec s = scaled(tilt(-3.0), -20.0);
        std::vector<std::vector<double>> pw, mg;
        for (int seed = 0; seed < 20; ++seed) {
            std::mt19937 rng(100 + seed);
            Capture in(bins), ref(bins);
            for (int f = 0; f < 35; ++f) add(in, s, rng, Gate::Broadband), add(ref, s, rng, Gate::Broadband);
            pw.push_back(minus(bandsDb(ref, false), bandsDb(in, false)));
            mg.push_back(minus(bandsDb(ref, true), bandsDb(in, true)));
        }
        auto spread = [&](const std::vector<std::vector<double>>& v, size_t b) {
            double m = 0.0, q = 0.0;
            for (const auto& x : v) m += x[b];
            m /= static_cast<double>(v.size());
            for (const auto& x : v) q += (x[b] - m) * (x[b] - m);
            return std::sqrt(q / static_cast<double>(v.size() - 1));
        };
        std::printf("B. Stationary material, 3 s captures, 20 seeds: standard deviation of the EQ curve (truth 0 dB)\n");
        for (size_t b : {size_t{0}, nb / 2, nb - 1})
            std::printf("   at %5.0f Hz: power %.2f dB, magnitude %.2f dB\n", centres[b], spread(pw, b), spread(mg, b));
    }
    // C. Gating: 20 s of programme, then 20 s of silence (a -140 dB floor) with a 50 Hz hum line at
    // -60 dB in its bin. Broadband gate: mean bin power over 20 Hz-20 kHz >= -90 dB. Peak gate: the
    // loudest bin >= -90 dB. The average, against the programme alone.
    {
        const Spec prog = scaled(tilt(-3.0), -30.0);
        size_t b50 = 0;
        for (size_t b = 0; b < nb; ++b)
            if (std::abs(std::log2(centres[b] / 50.0)) < 1.0 / 6.0) b50 = b;
        for (double humDb : {-80.0, -70.0, -60.0, -50.0})
        for (Gate g : {Gate::Broadband, Gate::Peak}) {
            Spec hum(bins, std::pow(10.0, -14.0));
            hum[static_cast<size_t>(std::lround(50.0 / binHz))] = std::pow(10.0, humDb / 10.0);
            std::mt19937 rng(5);
            Capture c(bins);
            for (int f = 0; f < 234; ++f) add(c, prog, rng, Gate::None);
            const Capture alone = c;
            for (int f = 0; f < 234; ++f) add(c, hum, rng, g);
            const auto with = bandsDb(c, false), without = bandsDb(alone, false);
            std::printf("C. hum %+3.0f dB, %s gate: %3.0f of 234 silent frames counted; %3.0f Hz %+.2f dB, %5.0f Hz %+.2f dB\n",
                        humDb, g == Gate::Broadband ? "broadband" : "peak     ", c.counted - alone.counted, centres[b50],
                        with[b50] - without[b50], centres[nb / 2], with[nb / 2] - without[nb / 2]);
        }
    }
}
