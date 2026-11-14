#include <doctest/doctest.h>

#include "accuracy.hpp"

using namespace amq;

TEST_CASE("accuracy harness runs a small grid and reports sane numbers") {
    accuracy::SweepConfig cfg;
    cfg.shapes = {dsp::Shape::Bell, dsp::Shape::HighShelf, dsp::Shape::HighCut, dsp::Shape::AllPass};
    cfg.slopes = {dsp::Slope::Db12, dsp::Slope::Db24};
    cfg.frequenciesHz = {100.0, 1000.0};
    cfg.gainsDb = {-6.0, 6.0};
    cfg.qs = {0.707, 2.0};
    cfg.sampleRates = {48000.0, 96000.0};
    cfg.gridPoints = 128;
    cfg.threads = 2;

    const auto r = accuracy::runSweep(cfg);
    // Bell, High Shelf: 2 slopes x 2 f x 2 g x 2 Q x 2 fs = 32 each; High Cut, All Pass: 16 each.
    CHECK(r.cases.size() == 32 + 32 + 16 + 16);
    for (const auto& c : r.cases) {
        CHECK(c.evaluated);
        CHECK(c.maxErrorDb >= 0.0);
        if (c.spec.shape == dsp::Shape::AllPass) CHECK(c.allPassPhaseErrorDeg < 2.0);
    }
    const auto report = accuracy::formatReport(r, {});
    CHECK(report.find("Am-Q Phase 0 filter accuracy report") != std::string::npos);
    const auto csv = accuracy::formatCsv(r);
    CHECK(csv.find("shape,slope") == 0);
}
