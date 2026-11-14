#include "accuracy.hpp"

#include "amq/dsp/conventions.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <thread>

namespace amq::accuracy {

using namespace amq::dsp;

SweepConfig defaultFullSweep() {
    SweepConfig c;
    for (int i = 0; i < kShapeCount; ++i) c.shapes.push_back(static_cast<Shape>(i));
    for (int i = 0; i < kSlopeCount; ++i) c.slopes.push_back(static_cast<Slope>(i));
    // 1/3-octave steps from 10 Hz, plus the 30 kHz top of the range (R8).
    for (double f = 10.0; f < kMaxBandFrequencyHz; f *= std::exp2(1.0 / 3.0)) c.frequenciesHz.push_back(f);
    c.frequenciesHz.push_back(kMaxBandFrequencyHz);
    c.gainsDb = {-30.0, -15.0, -6.0, -1.0, 1.0, 6.0, 15.0, 30.0};
    c.qs = {0.1, 0.707, 1.0, 3.0, 10.0, 40.0};
    c.sampleRates = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0};
    return c;
}

CaseResult evaluateCase(const BandSpec& spec, double sampleRate, const SweepConfig& cfg) {
    CaseResult r;
    r.spec = spec;
    r.sampleRate = sampleRate;

    const AnalogPrototype proto = designAnalogPrototype(spec);
    if (!proto.supported) return r;
    DigitalFilter dig;
    if (cfg.method == DesignMethod::MatchedRefined && cfg.extraMode != 0 && proto.shape != Shape::AllPass) {
        RefineOptions ro;
        ro.extraSections = cfg.extraMode == 1 ? 0 : 1;
        ro.extraOnlyWhenNeeded = cfg.extraMode != 2;
        dig = designRefined(proto, sampleRate, ro);
    } else {
        dig = designDigital(proto, sampleRate, cfg.method);
    }
    r.protoSections = static_cast<int>(proto.sections.size());
    r.digitalSections = static_cast<int>(dig.sections.size());
    r.r12TolDb = r12ToleranceDb(spec.frequencyHz, proto.q, sampleRate);
    r.clampedSections = dig.clampedSections;

    const double nyquist = 0.5 * sampleRate;
    const double limit = cfg.limitFraction * nyquist;
    const double lo = kMinBandFrequencyHz;

    auto check = [&](double hz) {
        const double am = std::abs(proto.evaluateHz(hz));
        const double dm = std::abs(dig.responseHz(hz));
        const double a = linearToDbFloored(am, cfg.floorDb);
        const double d = linearToDbFloored(dm, cfg.floorDb);
        const double e = std::abs(d - a);
        const double ea = std::abs(linearToDbFloored(dm, cfg.audibleFloorDb) - linearToDbFloored(am, cfg.audibleFloorDb));
        if (!(ea <= r.maxErrorAudibleDb)) r.maxErrorAudibleDb = std::isnan(ea) ? 1e9 : ea;
        const double aRaw = linearToDb(am);
        if (aRaw >= cfg.tierDb) {
            const double et = std::abs(linearToDb(dm) - aRaw);
            if (!(et <= r.maxErrorUpperTierDb)) r.maxErrorUpperTierDb = std::isnan(et) ? 1e9 : et;
        } else if (!(e <= r.maxErrorLowerTierDb)) {
            r.maxErrorLowerTierDb = std::isnan(e) ? 1e9 : e;
        }
        if (!(e <= r.maxErrorDb)) {  // also catches NaN
            r.maxErrorDb = std::isnan(e) ? 1e9 : e;
            r.worstHz = hz;
            r.worstAnalogDb = linearToDb(am);
        }
    };

    const int n = std::max(cfg.gridPoints, 2);
    const double ratio = std::log(limit / lo);
    for (int i = 0; i < n; ++i) check(lo * std::exp(ratio * i / (n - 1)));
    // Dense points within +/-2 octaves of the centre, where peaks and notches live,
    // plus a finer window whose step shrinks with the highest section Q so narrow
    // resonances cannot fall between grid points.
    for (int k = -96; k <= 96; ++k) {
        const double hz = spec.frequencyHz * std::exp2(k / 48.0);
        if (hz >= lo && hz <= limit) check(hz);
    }
    double qMax = 0.5;
    for (const auto& s : proto.sections)
        if (s.a2 != 0.0 && s.a1 > 0.0) qMax = std::max(qMax, std::sqrt(s.a0 * s.a2) / s.a1);
    // Notch zeros: very fine points where the response crosses many dB in a narrow band.
    for (const auto& s : proto.sections) {
        if (!(s.b1 == 0.0 && s.b0 > 0.0 && s.b2 > 0.0)) continue;
        const double fz = std::sqrt(s.b0 / s.b2) * spec.frequencyHz;
        for (int k = -120; k <= 120; ++k) {
            const double hz = fz * std::exp2(k / 800.0);
            if (hz >= lo && hz <= limit) check(hz);
        }
    }
    const double fineStep = 1.0 / (16.0 * qMax);
    if (fineStep < 1.0 / 48.0) {
        const double span = std::max(0.25, 8.0 / qMax);
        for (double oct = -span; oct <= span; oct += fineStep) {
            const double hz = spec.frequencyHz * std::exp2(oct);
            if (hz >= lo && hz <= limit) check(hz);
        }
    }
    r.evaluated = true;

    if (spec.shape == Shape::AllPass && spec.frequencyHz < limit) {
        const double pa = std::arg(proto.evaluateHz(spec.frequencyHz));
        const double pd = std::arg(dig.responseHz(spec.frequencyHz));
        double diff = std::remainder(pd - pa, kTwoPi);
        r.allPassPhaseErrorDeg = std::abs(diff) * 180.0 / kPi;
    }
    return r;
}

namespace {

std::vector<std::pair<BandSpec, double>> enumerate(const SweepConfig& cfg) {
    std::vector<std::pair<BandSpec, double>> out;
    for (const auto shape : cfg.shapes) {
        for (const auto slope : cfg.slopes) {
            if (!slopeAllowed(shape, slope)) continue;
            if (!shapeUsesSlope(shape) && slope != cfg.slopes.front()) continue;
            const std::vector<double> gains = shapeHasGain(shape) ? cfg.gainsDb : std::vector<double>{0.0};
            const std::vector<double> qs = shapeUsesQ(shape) ? cfg.qs : std::vector<double>{1.0};
            for (const double f : cfg.frequenciesHz)
                for (const double g : gains)
                    for (const double q : qs)
                        for (const double fs : cfg.sampleRates)
                            out.push_back({BandSpec{shape, f, g, q, slope}, fs});
        }
    }
    return out;
}

}  // namespace

SweepResult runSweep(const SweepConfig& cfg) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto work = enumerate(cfg);
    SweepResult res;
    res.config = cfg;
    res.cases.resize(work.size());

    unsigned threads = cfg.threads > 0 ? static_cast<unsigned>(cfg.threads) : std::thread::hardware_concurrency();
    threads = std::max(1u, threads);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i = next.fetch_add(1); i < work.size(); i = next.fetch_add(1))
            res.cases[i] = evaluateCase(work[i].first, work[i].second, cfg);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();

    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
namespace {

// Revised R12 (area 01): per-band tolerance; TBD bands are counted separately.
bool r12Pass(const CaseResult& c) { return !std::isnan(c.r12TolDb) && c.maxErrorDb <= c.r12TolDb + 1e-12; }
bool r12Tbd(const CaseResult& c) { return std::isnan(c.r12TolDb); }

struct Stats {
    size_t count = 0, pass = 0, passAudible = 0, r12Count = 0, r12Passes = 0, tbd = 0, extra = 0;
    double r12WorstExcess = 0.0, tbdMax = 0.0;
    double maxErr = 0.0, maxErrAudible = 0.0;
    const CaseResult* worst = nullptr;
    void add(const CaseResult& c, double target) {
        ++count;
        if (c.maxErrorDb <= target) ++pass;
        if (c.maxErrorAudibleDb <= target) ++passAudible;
        if (c.digitalSections > c.protoSections) ++extra;
        if (r12Tbd(c)) {
            ++tbd;
            tbdMax = std::max(tbdMax, c.maxErrorDb);
        } else {
            ++r12Count;
            if (r12Pass(c)) ++r12Passes;
        }
        maxErrAudible = std::max(maxErrAudible, c.maxErrorAudibleDb);
        if (!worst || c.maxErrorDb > maxErr) {
            maxErr = c.maxErrorDb;
            worst = &c;
        }
    }
};

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string describe(const CaseResult& c) {
    std::ostringstream o;
    o << shapeName(c.spec.shape) << ", " << slopeName(c.spec.slope) << " dB/oct, f0 "
      << fmt("%.1f", c.spec.frequencyHz) << " Hz, gain " << fmt("%+.0f", c.spec.gainDb) << " dB, Q "
      << fmt("%g", c.spec.q) << ", fs " << fmt("%.1f", c.sampleRate / 1000.0) << " kHz (worst at "
      << fmt("%.0f", c.worstHz) << " Hz; R12 tol " << (std::isnan(c.r12TolDb) ? std::string("TBD") : fmt("%.1f", c.r12TolDb))
      << " dB; " << c.digitalSections << "/" << c.protoSections << " sections)";
    return o.str();
}

std::string passPct(const Stats& s) {
    return s.count ? fmt("%.1f %%", 100.0 * static_cast<double>(s.pass) / static_cast<double>(s.count)) : "-";
}

const char* centreBucket(const CaseResult& c) {
    const double r = c.spec.frequencyHz / (0.5 * c.sampleRate);
    if (r < 0.1) return "a: f0 < 0.1 Nyq";
    if (r < 0.25) return "b: 0.1-0.25 Nyq";
    if (r < 0.5) return "c: 0.25-0.5 Nyq";
    if (r < 0.9) return "d: 0.5-0.9 Nyq";
    if (r < 1.0) return "e: 0.9-1.0 Nyq";
    return "f: f0 >= Nyq";
}

const char* qBucket(const CaseResult& c) {
    if (!shapeUsesQ(c.spec.shape)) return "n/a";
    if (c.spec.q <= 1.0) return "Q <= 1";
    if (c.spec.q <= 5.0) return "1 < Q <= 5";
    return "Q > 5";
}

void sectionPerShape(std::ostringstream& o, const SweepResult& r) {
    const double target = r.config.targetDb;
    std::map<int, Stats> byShape;
    size_t unsupported = 0;
    for (const auto& c : r.cases) {
        if (!c.evaluated) { ++unsupported; continue; }
        byShape[static_cast<int>(c.spec.shape)].add(c, target);
    }
    o << "| Shape | Cases | **Revised R12 pass** | TBD cases (Q > 3, f0 > 0.9 Nyq): max error | Uses correction section | Pass <= "
      << fmt("%.2f", target) << " dB everywhere | Max error (dB) | Worst case |\n";
    o << "| --- | ---: | ---: | --- | ---: | ---: | ---: | --- |\n";
    auto pct = [](size_t a, size_t n) { return n ? fmt("%.3f %%", 100.0 * static_cast<double>(a) / static_cast<double>(n)) : std::string("-"); };
    Stats all;
    for (auto& [k, s] : byShape) {
        o << "| " << shapeName(static_cast<Shape>(k)) << " | " << s.count << " | " << pct(s.r12Passes, s.r12Count) << " ("
          << (s.r12Count - s.r12Passes) << " miss) | " << s.tbd << ": " << fmt("%.2f", s.tbdMax) << " dB | "
          << pct(s.extra, s.count) << " | " << passPct(s) << " | " << fmt("%.3f", s.maxErr) << " | "
          << (s.worst ? describe(*s.worst) : "") << " |\n";
        all.count += s.count;
        all.pass += s.pass;
        all.r12Count += s.r12Count;
        all.r12Passes += s.r12Passes;
        all.tbd += s.tbd;
        all.extra += s.extra;
        all.tbdMax = std::max(all.tbdMax, s.tbdMax);
        if (s.maxErr > all.maxErr) { all.maxErr = s.maxErr; all.worst = s.worst; }
    }
    o << "| **All** | " << all.count << " | **" << pct(all.r12Passes, all.r12Count) << "** (" << (all.r12Count - all.r12Passes)
      << " miss) | " << all.tbd << ": " << fmt("%.2f", all.tbdMax) << " dB | " << pct(all.extra, all.count) << " | "
      << passPct(all) << " | " << fmt("%.3f", all.maxErr) << " | |\n\n";
    if (unsupported) o << "Not evaluated (Brickwall, a Phase 2 design): " << unsupported << " cases.\n\n";
}

}  // namespace

std::string formatReport(const SweepResult& m, const std::vector<NamedResult>& comparisons) {
    std::ostringstream o;
    const auto& cfg = m.config;
    const double target = cfg.targetDb;
    o << "# Am-Q Phase 0 filter accuracy report\n\n";
    o << "Digital magnitude vs analog prototype, max |error| below " << fmt("%.2f", cfg.limitFraction)
      << " x Nyquist. Target (R12): <= " << fmt("%.2f", target) << " dB. Magnitudes are floored at "
      << fmt("%.0f", cfg.floorDb) << " dB before comparison (secondary columns: floored at "
      << fmt("%.0f", cfg.audibleFloorDb) << " dB, i.e. only levels that matter audibly). Grid: " << cfg.gridPoints
      << " log points from 10 Hz, 1/48-octave points within +/-2 octaves of f0, and a finer window (step 1/(16 Q_max) octave) for high-Q sections.\n\n";
    o << "Sweep: " << cfg.shapes.size() << " shapes, " << cfg.slopes.size() << " slopes, "
      << cfg.frequenciesHz.size() << " centre frequencies (10 Hz-30 kHz), " << cfg.gainsDb.size()
      << " gains, " << cfg.qs.size() << " Q values, sample rates";
    for (double fs : cfg.sampleRates) o << " " << fmt("%g", fs / 1000.0) << "k";
    o << ". " << m.cases.size() << " cases in " << fmt("%.1f", m.seconds) << " s.\n\n";
    o << "Revised R12 (area 01, 2026-11-14): <= 0.1 dB below 0.9 x Nyquist for f0 <= 0.9 x Nyquist; 1.1 dB for "
         "f0 > 0.9 x Nyquist with Q <= 3; TBD for f0 > 0.9 x Nyquist with Q > 3 (reported separately); levels below "
         "-60 dB only need to stay below -60 dB.\n\n";
    o << "Q convention: " << qMappingNotes() << "\n\n";

    o << "## Default design (matched + joint minimax refinement), by shape\n\n";
    sectionPerShape(o, m);
    for (const auto& c : comparisons) {
        o << "## " << c.name << " (comparison), by shape\n\n";
        sectionPerShape(o, *c.result);
    }

    // Region table: centre bucket x Q bucket, matched design, excluding All Pass
    // (magnitude 1 by construction; designed bilinear).
    o << "## Matched design by region (All Pass excluded)\n\n";
    std::map<std::string, std::map<std::string, Stats>> region;
    std::map<std::string, Stats> perRate;
    size_t clampedCases = 0;
    for (const auto& c : m.cases) {
        if (!c.evaluated || c.spec.shape == Shape::AllPass) continue;
        region[centreBucket(c)][qBucket(c)].add(c, target);
        perRate[fmt("%6.1f kHz", c.sampleRate / 1000.0)].add(c, target);
        if (c.clampedSections) ++clampedCases;
    }
    const char* qCols[] = {"Q <= 1", "1 < Q <= 5", "Q > 5", "n/a"};
    o << "Each cell: pass rate / max error (dB). \"n/a\" = shapes without Q (Flat Tilt).\n\n";
    o << "| Centre f0 | Q <= 1 | 1 < Q <= 5 | Q > 5 | n/a |\n| --- | --- | --- | --- | --- |\n";
    for (auto& [rb, row] : region) {
        o << "| " << rb.substr(3) << " |";
        for (const char* qc : qCols) {
            auto it = row.find(qc);
            if (it == row.end()) o << " - |";
            else o << " " << passPct(it->second) << " / " << fmt("%.3f", it->second.maxErr) << " |";
        }
        o << "\n";
    }
    o << "\n## Matched design by sample rate (All Pass excluded)\n\n| Sample rate | Cases | Pass | Max error (dB) |\n| --- | ---: | ---: | ---: |\n";
    for (auto& [k, s] : perRate)
        o << "| " << k << " | " << s.count << " | " << passPct(s) << " | " << fmt("%.3f", s.maxErr) << " |\n";
    o << "\nCases where at least one section's 3-point match was unrealisable and clamped: " << clampedCases << "\n\n";

    // Shape x centre bucket pass table.
    o << "## Matched design pass rate by shape and centre frequency\n\n| Shape |";
    const char* cbs[] = {"a: f0 < 0.1 Nyq", "b: 0.1-0.25 Nyq", "c: 0.25-0.5 Nyq", "d: 0.5-0.9 Nyq", "e: 0.9-1.0 Nyq", "f: f0 >= Nyq"};
    for (const char* cb : cbs) o << " " << (cb + 3) << " |";
    o << "\n| --- |";
    for (size_t i = 0; i < std::size(cbs); ++i) o << " --- |";
    o << "\n";
    std::map<int, std::map<std::string, Stats>> shapeRegion;
    for (const auto& c : m.cases)
        if (c.evaluated) shapeRegion[static_cast<int>(c.spec.shape)][centreBucket(c)].add(c, target);
    for (auto& [k, row] : shapeRegion) {
        o << "| " << shapeName(static_cast<Shape>(k)) << " |";
        for (const char* cb : cbs) {
            auto it = row.find(cb);
            if (it == row.end()) o << " - |";
            else o << " " << passPct(it->second) << " / " << fmt("%.2f", it->second.maxErr) << " |";
        }
        o << "\n";
    }

    // All Pass phase.
    double apMax = 0.0;
    size_t apCount = 0, apPass = 0;
    for (const auto& c : m.cases)
        if (c.allPassPhaseErrorDeg >= 0.0) {
            ++apCount;
            if (c.allPassPhaseErrorDeg <= 2.0) ++apPass;
            apMax = std::max(apMax, c.allPassPhaseErrorDeg);
        }
    o << "\n## All Pass phase at the centre frequency (bilinear, pre-warped at f0)\n\n";
    o << "Cases with f0 below the limit: " << apCount << ", within 2 degrees: " << apPass
      << ", max error: " << fmt("%.2e", apMax) << " degrees.\n\n";

    // Worst cases.
    std::vector<const CaseResult*> sorted;
    for (const auto& c : m.cases)
        if (c.evaluated) sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b2) { return a->maxErrorDb > b2->maxErrorDb; });
    o << "## 25 worst matched cases\n\n| Error (dB) | Case |\n| ---: | --- |\n";
    for (size_t i = 0; i < std::min<size_t>(25, sorted.size()); ++i)
        o << "| " << fmt("%.3f", sorted[i]->maxErrorDb) << " | " << describe(*sorted[i]) << " |\n";
    return o.str();
}

std::string formatCsv(const SweepResult& r) {
    std::ostringstream o;
    o << "shape,slope,f0_hz,gain_db,q,fs,max_err_db,max_err_audible_db,max_err_upper_tier_db,max_err_lower_tier_db,worst_analog_db,r12_tol_db,proto_sections,digital_sections,worst_hz,clamped_sections,allpass_phase_err_deg,evaluated\n";
    for (const auto& c : r.cases) {
        o << shapeName(c.spec.shape) << ',' << slopeName(c.spec.slope) << ',' << fmt("%.3f", c.spec.frequencyHz) << ','
          << c.spec.gainDb << ',' << c.spec.q << ',' << c.sampleRate << ',' << fmt("%.6f", c.maxErrorDb) << ','
          << fmt("%.6f", c.maxErrorAudibleDb) << ',' << fmt("%.6f", c.maxErrorUpperTierDb) << ','
          << fmt("%.6f", c.maxErrorLowerTierDb) << ',' << fmt("%.2f", c.worstAnalogDb) << ',' << fmt("%.3f", c.r12TolDb) << ','
          << c.protoSections << ',' << c.digitalSections << ',' << fmt("%.1f", c.worstHz) << ',' << c.clampedSections << ',' << fmt("%.6f", c.allPassPhaseErrorDeg) << ','
          << (c.evaluated ? 1 : 0) << '\n';
    }
    return o.str();
}

}  // namespace amq::accuracy
