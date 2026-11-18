// Editor region map, reflow and value formats (docs/design/editor-spec.md §1, panels §2.3, §2.10).
#include "amq/ui/band_menu.hpp"
#include "amq/ui/display_format.hpp"
#include "amq/ui/editor_layout.hpp"
#include "amq/ui/graph_model.hpp"

#include <doctest/doctest.h>

using namespace amq;
using ui::EditorLayout;
using ui::PanelContent;
using ui::PanelMode;

namespace {
const std::string kM = ui::fmt::kMinus;
}

TEST_CASE("Region map at the board size: bars, graph, plot, gutter (spec §1.1)") {
    const auto L = EditorLayout::compute(1280, 760, {});
    CHECK(L.top.h == 44);
    CHECK(L.status.y == 716);
    CHECK(L.graph.y == 44);
    CHECK(L.graph.h == 672);
    CHECK(L.plot.x == 0);
    CHECK(L.plot.y == 24);
    CHECK(L.plot.w == 1180);
    CHECK(L.plot.h == 624);
    CHECK(L.panel.mode == PanelMode::Hidden);  // no band selected (P-8)
    // The graph geometry agrees: 20 Hz, 1 kHz and 20 kHz land on the board's gridlines.
    ui::GraphGeometry g;
    g.setSize(L.graph.w, L.graph.h);
    CHECK(g.xOfHz(20.0) == doctest::Approx(102.0).epsilon(0.01));
    CHECK(g.xOfHz(1000.0) == doctest::Approx(679.0).epsilon(0.005));
    CHECK(g.xOfHz(20000.0) == doctest::Approx(1120.0).epsilon(0.005));
    g.setRangeDb(18.0);
    CHECK(g.yOfDb(0.0) == doctest::Approx(336.0));
    CHECK(g.yOfDb(18.0) == doctest::Approx(24.0));
}

TEST_CASE("Band panel sizes by band state (panels §2.3)") {
    PanelContent c;
    c.band = true;
    auto L = EditorLayout::compute(1280, 760, c);
    CHECK(L.panel.mode == PanelMode::Full);
    CHECK(L.panel.rect.w == doctest::Approx(632));
    CHECK(L.panel.rect.h == doctest::Approx(166));
    CHECK(L.panel.rect.bottom() == doctest::Approx(672 - 32));
    CHECK(L.panel.rect.x == doctest::Approx((1280 - 632) / 2.0));
    // Dynamics knob columns widen to their captions (THRESHOLD 72, RELEASE and DENSITY 56), as the
    // board's flex columns do: 1080 and 1140 at the board's nominal 48 px, 1112 and 1180 here.
    c.dynamic = true;
    CHECK(EditorLayout::compute(1280, 760, c).panel.rect.w == doctest::Approx(1112));
    c.spectral = true;
    CHECK(EditorLayout::compute(1280, 760, c).panel.rect.w == doctest::Approx(1180));
    PanelContent cut;
    cut.band = true, cut.gain = false;
    CHECK(EditorLayout::compute(1280, 760, cut).panel.rect.w == doctest::Approx(528));
}

TEST_CASE("Reflow: wrapping, compact and strip modes, bar collapse (spec §1.2)") {
    PanelContent c;
    c.band = true, c.dynamic = true, c.spectral = true;
    auto L = EditorLayout::compute(1100, 760, c);  // 1180 + 48 > 1100
    CHECK(L.panel.wrapped);
    CHECK(L.panel.rect.w <= 1100 - 48);
    L = EditorLayout::compute(1180, 560, c);  // graph 472 high: compact
    CHECK(L.panel.mode == PanelMode::Compact);
    CHECK(L.panel.knobLg == 64);
    CHECK(L.panel.rect.bottom() == doctest::Approx(L.graph.h - 16));
    L = EditorLayout::compute(640, 360, c);  // graph 272 high: strip
    CHECK(L.panel.mode == PanelMode::Strip);
    CHECK(L.panel.rect.h == 44);
    CHECK(L.hidePrevNext);
    CHECK(L.hideAutoGain);
    CHECK(L.hideAnalyzerCaption);
    L = EditorLayout::compute(3840, 2160, c);
    CHECK(L.panel.mode == PanelMode::Full);
    CHECK(L.panel.rect.w == doctest::Approx(1180));  // the panel never scales with the window
    CHECK(L.plot.w == 3740);
    CHECK_FALSE(L.matchGlyphOnly);
}

TEST_CASE("Value formats (panels §2.10)") {
    using namespace ui::fmt;
    CHECK(hz(32.4) == "32 Hz");
    CHECK(hz(180) == "180 Hz");
    CHECK(hz(1820) == "1.82 kHz");
    CHECK(hz(9000) == "9.00 kHz");
    CHECK(hz(14500) == "14.5 kHz");
    CHECK(hzAxis(20) == "20");
    CHECK(hzAxis(1000) == "1k");
    CHECK(hzAxis(20000) == "20k");
    CHECK(hzRange(120, 270) == "120\xE2\x80\x93" "270 Hz");
    CHECK(hzRange(900, 3600) == "0.9\xE2\x80\x93" "3.6 kHz");
    CHECK(hzRange(6000, 14000) == "6.0\xE2\x80\x93" "14 kHz");
    CHECK(db(4.5) == "+4.5 dB");
    CHECK(db(-3.5) == kM + "3.5 dB");
    CHECK(db(0.0) == "+0.0 dB");
    CHECK(db(-0.04) == "+0.0 dB");  // no negative zero
    CHECK(dbfs(-20) == kM + "20.0 dB");
    CHECK(q(0.707) == "0.71");
    CHECK(q(4) == "4.00");
    CHECK(q(12.5) == "12.5");
    CHECK(slope(3) == "24 dB/oct");
    CHECK(slope(9) == "Brickwall");
    CHECK(pct(100) == "100 %");
    CHECK(latency(4160) == "4,160 smp");
    CHECK(latency(65600) == "65,600 smp");
    CHECK(latency(320) == "320 smp");
    CHECK(count(1284) == "1,284");
    CHECK(dbAxis(12, true) == "+12");
    CHECK(dbAxis(0, true) == "0");
    CHECK(dbAxis(-6, true) == kM + "6");
    CHECK(pan(0, false) == "C");
    CHECK(pan(-12, false) == "12.0 L");
    CHECK(pan(30, true) == "30.0 S");
}

#include "amq/params/mappings.hpp"

TEST_CASE("Host-facing text equals the editor's formats (spec C-29)") {
    using namespace params;
    const auto& freq = *specAt(bandParam(0, BandField::Freq));
    const auto& gain = *specAt(bandParam(0, BandField::Gain));
    const auto& q = *specAt(bandParam(0, BandField::Q));
    for (double hz : {12.3, 32.4, 180.0, 999.0, 1000.0, 1820.0, 9990.0, 14500.0, 29000.0})
        CHECK(formatPlain(freq, std::log2(hz)) == ui::fmt::hz(hz));
    for (double db : {-30.0, -3.5, -0.04, 0.0, 4.5, 12.25})
        CHECK(formatPlain(gain, db) == ui::fmt::db(db));
    for (double qq : {0.1, 0.707, 4.0, 12.5})
        CHECK(formatPlain(q, positionFromQ(qq)) == ui::fmt::q(qq));
    CHECK(*parsePlain(gain, kM + "3.5 dB") == doctest::Approx(-3.5));  // U+2212 parses
    CHECK(formatPlain(*specAt(bandParam(0, BandField::Attack)), 50.0) == "50 %");
}
