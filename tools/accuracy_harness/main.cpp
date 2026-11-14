// amq_accuracy_harness: runs the Phase 0 filter-accuracy sweep and writes a report.
//
// Usage: amq_accuracy_harness [--out report.md] [--csv cases.csv] [--quick] [--no-bilinear]
//                             [--threads N] [--strict]
//   --quick       a reduced grid for a fast smoke run
//   --strict      exit code 1 if any matched case misses the target
#include "accuracy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace amq;
    std::string outPath = "accuracy_report.md";
    std::string csvPath;
    bool quick = false, withBilinear = true, strict = false;
    int threads = 0;
    int extraMode = 0;
    bool defaultOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else if (a == "--quick") quick = true;
        else if (a == "--no-bilinear") withBilinear = false;
        else if (a == "--strict") strict = true;
        else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--order-n") extraMode = 1;
        else if (a == "--always-extra") extraMode = 2;
        else if (a == "--default-only") defaultOnly = true;
        else if (a == "--bench") {
            // Single-thread design cost over the full grid at 48 kHz, per method.
            auto cfg = accuracy::defaultFullSweep();
            for (int extra : {0, 1}) {
                for (auto method : {dsp::DesignMethod::MatchedThreePoint, dsp::DesignMethod::MatchedLeastSquares,
                                    dsp::DesignMethod::MatchedRefined}) {
                    if (extra && method != dsp::DesignMethod::MatchedRefined) continue;
                    std::vector<double> us;
                    for (int sh = 0; sh < dsp::kShapeCount; ++sh)
                        for (int sl = 0; sl < 9; ++sl)
                            for (size_t fi = 0; fi < cfg.frequenciesHz.size(); fi += 3)
                                for (double g : {-15.0, 6.0})
                                    for (double q : {0.707, 10.0}) {
                                        const auto p = dsp::designAnalogPrototype({static_cast<dsp::Shape>(sh), cfg.frequenciesHz[fi], g, q, static_cast<dsp::Slope>(sl)});
                                        const auto t0 = std::chrono::steady_clock::now();
                                        dsp::DigitalFilter d;
                                        if (extra) { dsp::RefineOptions ro; ro.extraSections = 0; d = dsp::designRefined(p, 48000.0, ro); }
                                        else d = dsp::designDigital(p, 48000.0, method);
                                        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                                    }
                    std::sort(us.begin(), us.end());
                    std::printf("method %d %s: n %zu  p50 %.1f us  p90 %.1f us  p99 %.1f us  max %.1f us\n",
                                static_cast<int>(method), extra ? "order N only" : "default", us.size(), us[us.size() / 2], us[us.size() * 9 / 10],
                                us[us.size() * 99 / 100], us.back());
                }
            }
            return 0;
        }
        else if (a == "--case" && i + 6 < argc) {
            // --case <shapeId> <slopeId> <f0> <gainDb> <q> <fs> : per-section dump
            dsp::BandSpec spec{static_cast<dsp::Shape>(std::atoi(argv[i + 1])), std::atof(argv[i + 3]),
                               std::atof(argv[i + 4]), std::atof(argv[i + 5]),
                               static_cast<dsp::Slope>(std::atoi(argv[i + 2]))};
            const double fs = std::atof(argv[i + 6]);
            const auto proto = dsp::designAnalogPrototype(spec);
            for (auto method : {dsp::DesignMethod::MatchedRefined, dsp::DesignMethod::MatchedLeastSquares,
                                dsp::DesignMethod::MatchedThreePoint, dsp::DesignMethod::BilinearPrewarp}) {
                const auto d = dsp::designDigital(proto, fs, method);
                // Fine scan: 20000 log points from 10 Hz to 0.9 Nyquist, floor -60 dB.
                double worst = 0.0, worstHz = 0.0;
                const double lim = 0.45 * fs;
                for (int k = 0; k <= 20000; ++k) {
                    const double hz = 10.0 * std::pow(lim / 10.0, k / 20000.0);
                    const double ea = std::max(-60.0, 20 * std::log10(std::abs(proto.evaluateHz(hz))));
                    const double ed = std::max(-60.0, 20 * std::log10(std::abs(d.responseHz(hz))));
                    if (std::abs(ea - ed) > worst) { worst = std::abs(ea - ed); worstHz = hz; }
                }
                std::printf("method %d, %zu sections, clamped %d, fit-grid max error %.4f dB, fine-scan %.4f dB at %.1f Hz\n",
                            static_cast<int>(method), d.sections.size(), d.clampedSections,
                            dsp::fitGridMaxErrorDb(proto, d), worst, worstHz);
                for (size_t k = 0; k < proto.sections.size(); ++k) {
                    const auto& s = proto.sections[k];
                    const auto& b = d.sections[k];
                    std::printf(" s%zu analog b=[%g %g %g] a=[%g %g %g]  digital b=[%g %g %g] a=[1 %g %g]\n", k,
                                s.b0, s.b1, s.b2, s.a0, s.a1, s.a2, b.b0, b.b1, b.b2, b.a1, b.a2);
                    for (double hz : {10.0, 100.0, spec.frequencyHz, 0.45 * fs}) {
                        const double w = 2.0 * 3.141592653589793 * hz / fs;
                        const double ea = 20 * std::log10(std::abs(s.evaluate(hz / spec.frequencyHz)));
                        const double ed = 20 * std::log10(std::abs(b.response(w)));
                        std::printf("    %8.1f Hz: analog %9.3f dB digital %9.3f dB\n", hz, ea, ed);
                    }
                }
            }
            return 0;
        }
        else {
            std::cerr << "unknown argument: " << a << "\n";
            return 2;
        }
    }

    accuracy::SweepConfig cfg = accuracy::defaultFullSweep();
    cfg.threads = threads;
    cfg.extraMode = extraMode;
    if (quick) {
        cfg.frequenciesHz = {30.0, 300.0, 3000.0, 10000.0, 18000.0};
        cfg.gainsDb = {-12.0, 12.0};
        cfg.qs = {0.707, 4.0};
        cfg.sampleRates = {44100.0, 96000.0};
    }

    std::cout << "Running default (matched + joint refinement) sweep...\n";
    cfg.method = dsp::DesignMethod::MatchedRefined;
    const auto matched = accuracy::runSweep(cfg);
    accuracy::SweepResult leastSquares, threePoint, bil;
    std::vector<accuracy::NamedResult> comparisons;
    if (!defaultOnly) {
        std::cout << "Running matched (least-squares, per section) sweep...\n";
        auto lcfg = cfg;
        lcfg.method = dsp::DesignMethod::MatchedLeastSquares;
        leastSquares = accuracy::runSweep(lcfg);
        std::cout << "Running matched (three-point, Vicanek 2016) sweep...\n";
        auto tcfg = cfg;
        tcfg.method = dsp::DesignMethod::MatchedThreePoint;
        threePoint = accuracy::runSweep(tcfg);
        comparisons.push_back({"Matched, least-squares per section (previous default)", &leastSquares});
        comparisons.push_back({"Matched, three-point (Vicanek 2016 baseline)", &threePoint});
    }
    if (withBilinear && !defaultOnly) {
        std::cout << "Running bilinear sweep...\n";
        auto bcfg = cfg;
        bcfg.method = dsp::DesignMethod::BilinearPrewarp;
        bil = accuracy::runSweep(bcfg);
        comparisons.push_back({"Bilinear with pre-warp at f0 (fallback)", &bil});
    }

    const std::string report = accuracy::formatReport(matched, comparisons);
    std::ofstream(outPath) << report;
    if (!csvPath.empty()) std::ofstream(csvPath) << accuracy::formatCsv(matched);
    std::cout << report << "\nReport written to " << outPath << "\n";

    size_t fails = 0;
    for (const auto& c : matched.cases)
        if (c.evaluated && !std::isnan(c.r12TolDb) && c.maxErrorDb > c.r12TolDb) ++fails;
    std::cout << "Cases missing revised R12 (TBD excluded): " << fails << " of " << matched.cases.size() << "\n";
    return (strict && fails) ? 1 : 0;
}
