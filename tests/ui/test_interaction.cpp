// Scripted interaction tests for every graph gesture (area 13 task "Write scripted interaction tests
// for every gesture"; R2, R7, R9-R11, R15-R17; area 15 R5 focus traversal).
#include "amq/dsp/analog_prototype.hpp"
#include "amq/params/mappings.hpp"
#include "amq/ui/graph_model.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

class Host : public ui::EditController {
public:
    params::ParameterTable t;
    int begins = 0, ends = 0;
    std::map<params::ParamIndex, int> open;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override { ++begins, ++open[i]; }
    void performEdit(params::ParamIndex i, double v) override {
        CHECK(open[i] > 0);
        t.set(i, v, ParamSource::Gui);
    }
    void endEdit(params::ParamIndex i) override { ++ends, --open[i]; }
    bool balanced() const {
        for (const auto& [i, n] : open)
            if (n) return false;
        return begins == ends;
    }
};

struct Rig {
    Host h;
    ui::GraphGeometry g;
    ui::GraphInteraction m{h, g};
    Rig() { g.setSize(1000.0f, 600.0f); }
    int add(double hz, double db) { return m.createBandAt(g.xOfHz(hz), g.yOfDb(db)); }
    double v(int b, BandField f) const { return h.t.value(params::bandParam(b, f)); }
    void click(int b, ui::Modifiers mod = {}) {
        const auto n = m.node(b);
        m.pointerDown(n.x, n.y, ui::Button::Left, mod, 1);
        m.pointerUp(n.x, n.y, mod);
    }
    void drag(float x0, float y0, float x1, float y1, ui::Modifiers mod = {}) {
        m.pointerDown(x0, y0, ui::Button::Left, mod, 1);
        m.pointerDrag((x0 + x1) / 2, (y0 + y1) / 2, mod);
        m.pointerDrag(x1, y1, mod);
        m.pointerUp(x1, y1, mod);
    }
};

const ui::Modifiers kShift{true, false, false}, kAlt{false, true, false}, kCmd{false, false, true};

}  // namespace

TEST_CASE("Selection: click, Shift-click adds, Command-click toggles, a click inside a selection keeps one") {
    Rig r;
    r.add(100, 0), r.add(1000, 3), r.add(8000, -3);
    r.click(0);
    CHECK(r.m.selection() == std::vector<int>{0});
    r.click(1, kShift);
    CHECK(r.m.selection() == std::vector<int>{0, 1});
    CHECK(r.m.focused() == 1);
    r.click(2, kCmd);
    CHECK(r.m.selection() == std::vector<int>{0, 1, 2});
    r.click(0, kCmd);
    CHECK(r.m.selection() == std::vector<int>{1, 2});
    r.click(2);  // plain click inside the selection: on release, only that band
    CHECK(r.m.selection() == std::vector<int>{2});
    CHECK(r.h.balanced());
}

TEST_CASE("Lasso selects the nodes inside; Shift-lasso adds; a click on empty space clears") {
    Rig r;
    r.add(100, 0), r.add(200, 0), r.add(5000, 0);
    const float y = r.g.yOfDb(0);
    r.drag(r.g.xOfHz(70), y - 40, r.g.xOfHz(300), y + 40);
    CHECK(r.m.selection() == std::vector<int>{0, 1});
    r.drag(r.g.xOfHz(3000), y - 40, r.g.xOfHz(8000), y + 40, kShift);
    CHECK(r.m.selection() == std::vector<int>{0, 1, 2});
    r.m.pointerDown(r.g.xOfHz(1000), r.g.yOfDb(10), ui::Button::Left, {}, 1);
    r.m.pointerUp(r.g.xOfHz(1000), r.g.yOfDb(10), {});
    CHECK(r.m.selection().empty());
}

TEST_CASE("Multi-band drag moves every selected band by the same octaves and dB, in one gesture") {
    Rig r;
    r.add(100, 0), r.add(1000, 3);
    r.click(0), r.click(1, kShift);
    const auto n = r.m.node(1);
    const int before = r.h.begins;
    const float dx = 100.0f, dy = -50.0f;
    r.drag(n.x, n.y, n.x + dx, n.y + dy);
    CHECK(r.h.begins - before == 4);  // frequency and gain of two bands
    const double dOct = dx * r.g.octavesPerPixel();
    const double dDb = r.g.dbOfY(n.y + dy) - r.g.dbOfY(n.y);
    CHECK(std::log2(std::exp2(r.v(0, BandField::Freq)) / 100.0) == doctest::Approx(dOct).epsilon(1e-6));
    CHECK(std::log2(std::exp2(r.v(1, BandField::Freq)) / 1000.0) == doctest::Approx(dOct).epsilon(1e-6));
    CHECK(r.v(0, BandField::Gain) == doctest::Approx(0.0 + dDb).epsilon(1e-6));
    CHECK(r.v(1, BandField::Gain) == doctest::Approx(3.0 + dDb).epsilon(1e-6));
    CHECK(r.h.balanced());
}

TEST_CASE("Shift-drag is fine; Command-drag locks the axis; Alt-drag changes Q") {
    Rig r;
    r.add(1000, 0);
    auto n = r.m.node(0);
    r.drag(n.x, n.y, n.x + 100, n.y, kShift);
    CHECK(std::log2(std::exp2(r.v(0, BandField::Freq)) / 1000.0) ==
          doctest::Approx(10.0 * r.g.octavesPerPixel()).epsilon(1e-6));
    const double f1 = r.v(0, BandField::Freq);
    n = r.m.node(0);
    // Command at the press toggles the selection; held during the drag it locks the axis.
    r.m.pointerDown(n.x, n.y, ui::Button::Left, {}, 1);
    r.m.pointerDrag(n.x + 5, n.y - 40, kCmd);  // mostly vertical: x locked
    r.m.pointerDrag(n.x + 10, n.y - 80, kCmd);
    r.m.pointerUp(n.x + 10, n.y - 80, kCmd);
    CHECK(r.v(0, BandField::Freq) == f1);
    CHECK(r.v(0, BandField::Gain) > 1.0);
    const double g1 = r.v(0, BandField::Gain), q1 = r.v(0, BandField::Q);
    n = r.m.node(0);
    r.m.pointerDown(n.x, n.y, ui::Button::Left, kAlt, 1);  // Alt-click toggles Enabled...
    r.m.pointerDrag(n.x, n.y - 25, kAlt);                    // ...and an Alt-drag after it moves Q
    r.m.pointerDrag(n.x, n.y - 50, kAlt);
    r.m.pointerUp(n.x, n.y - 50, kAlt);
    CHECK(r.v(0, BandField::Gain) == g1);
    CHECK(r.v(0, BandField::Q) == doctest::Approx(q1 + 50 * ui::GraphInteraction::kDragQPerPx));
    CHECK(r.h.balanced());
}

TEST_CASE("Range handle: drag sets Dyn Range; Alt-click toggles Dynamics Enabled") {
    Rig r;
    r.add(1000, 3);
    r.h.t.set(params::bandParam(0, BandField::DynEnabled), 1, ParamSource::Preset);
    r.h.t.set(params::bandParam(0, BandField::DynRange), -6, ParamSource::Preset);
    auto n = r.m.node(0);
    REQUIRE(n.hasRangeHandle);
    CHECK(r.m.hitRangeHandle(n.x, n.rangeY) == 0);
    r.drag(n.x, n.rangeY, n.x, n.rangeY + 40);
    CHECK(r.v(0, BandField::DynRange) == doctest::Approx(-6 + (r.g.dbOfY(n.rangeY + 40) - r.g.dbOfY(n.rangeY))).epsilon(1e-6));
    CHECK(r.v(0, BandField::Gain) == 3.0);
    n = r.m.node(0);
    r.m.pointerDown(n.x, n.rangeY, ui::Button::Left, kAlt, 1);
    r.m.pointerUp(n.x, n.rangeY, kAlt);
    CHECK(r.v(0, BandField::DynEnabled) == 0.0);
    CHECK(r.h.balanced());
}

TEST_CASE("Wheel bindings (editor-spec §6.2): Q, Command gain, Alt range, Command+Alt slope, Shift fine; empty space zooms") {
    Rig r;
    r.add(1000, 0);
    r.add(80, 0);
    r.h.t.set(params::bandParam(1, BandField::Shape), static_cast<int>(dsp::Shape::LowCut), ParamSource::Preset);
    auto n = r.m.node(0);
    const double q = r.v(0, BandField::Q);
    r.m.wheel(n.x, n.y, 1, {});
    CHECK(r.v(0, BandField::Q) == doctest::Approx(q + ui::GraphInteraction::kWheelQStep));
    r.m.wheel(n.x, n.y, 1, kShift);  // fine Q
    CHECK(r.v(0, BandField::Q) == doctest::Approx(q + 1.1 * ui::GraphInteraction::kWheelQStep));
    r.m.wheel(n.x, n.y, 2, kCmd);
    CHECK(r.v(0, BandField::Gain) == doctest::Approx(2 * ui::GraphInteraction::kWheelGainDb));
    n = r.m.node(0);  // the node moved with its gain
    r.m.wheel(n.x, n.y, 1, {true, false, true});  // fine gain
    CHECK(r.v(0, BandField::Gain) == doctest::Approx(2.1 * ui::GraphInteraction::kWheelGainDb));
    n = r.m.node(0);  // the node moved with its gain
    r.m.wheel(n.x, n.y, -4, kAlt);
    CHECK(r.v(0, BandField::DynRange) == doctest::Approx(-4 * ui::GraphInteraction::kWheelGainDb));
    const ui::Modifiers kSlope{false, true, true};
    for (int k = 0; k < 20; ++k) r.m.wheel(n.x, n.y, 1, kSlope);  // a Bell never reaches Brickwall (node y: gain only)
    CHECK(r.v(0, BandField::Slope) == static_cast<int>(dsp::Slope::Db96));
    auto c = r.m.node(1);
    for (int k = 0; k < 20; ++k) r.m.wheel(c.x, c.y, 1, kSlope);  // a cut does
    CHECK(r.v(1, BandField::Slope) == static_cast<int>(dsp::Slope::Brickwall));
    r.m.clearSelection();
    const float x = r.g.xOfHz(2000);
    r.m.wheel(x, r.g.yOfDb(11), 3, {});  // nothing under the pointer, nothing selected: zoom
    CHECK(r.g.viewHi() / r.g.viewLo() < 1000.0);
    CHECK(r.g.hzOfX(x) == doctest::Approx(2000.0).epsilon(1e-6));  // around the pointer
    for (int k = 0; k < 30; ++k) r.m.wheel(x, r.g.yOfDb(11), -3, {});
    CHECK(r.g.viewLo() == ui::GraphGeometry::kMinHz);
    CHECK(r.g.viewHi() == ui::GraphGeometry::kMaxHz);
    CHECK(r.h.balanced());
}

TEST_CASE("Solo-level drag: a soloed band's vertical drag sets Solo Gain, not its gain (R11)") {
    Rig r;
    r.add(1000, 4);
    r.h.t.set(params::bandParam(0, BandField::Solo), 1, ParamSource::Preset);
    const auto n = r.m.node(0);
    r.m.pointerDown(n.x, n.y, ui::Button::Left, {}, 1);
    r.m.pointerDrag(n.x, n.y - 30, {});
    r.m.pointerDrag(n.x, n.y - 60, {});
    CHECK(r.m.soloDragActive());
    r.m.pointerUp(n.x, n.y - 60, {});
    CHECK(r.v(0, BandField::Gain) == doctest::Approx(4.0).epsilon(1e-6));
    CHECK(r.h.t.value(params::globalParam(params::GlobalParam::SoloGain)) ==
          doctest::Approx(r.g.dbOfY(n.y - 60) - r.g.dbOfY(n.y)).epsilon(1e-6));
}

TEST_CASE("Gain-Q coupling (off by default) narrows Q with |gain|; Escape cancels a drag") {
    Rig r;
    r.add(1000, 0);
    CHECK_FALSE(r.m.gainQCoupling);
    r.m.gainQCoupling = true;
    auto n = r.m.node(0);
    const double q0 = params::qFromPosition(r.v(0, BandField::Q));
    const float y12 = r.g.yOfDb(12);
    r.drag(n.x, n.y, n.x, y12);
    CHECK(params::qFromPosition(r.v(0, BandField::Q)) == doctest::Approx(q0 * 2.0).epsilon(1e-3));
    n = r.m.node(0);
    const double g = r.v(0, BandField::Gain), f = r.v(0, BandField::Freq);
    r.m.pointerDown(n.x, n.y, ui::Button::Left, {}, 1);
    r.m.pointerDrag(n.x + 80, n.y + 80, {});
    r.m.cancelDrag();
    CHECK(r.v(0, BandField::Gain) == g);
    CHECK(r.v(0, BandField::Freq) == f);
    CHECK(r.h.balanced());
}

TEST_CASE("Focus traversal by frequency wraps around; right-click targets the node or the selection") {
    Rig r;
    r.add(5000, 0), r.add(100, 0), r.add(1000, 0);
    r.m.clearSelection();
    r.m.focusNext(1);
    CHECK(r.m.focused() == 1);  // lowest frequency first
    r.m.focusNext(1);
    CHECK(r.m.focused() == 2);
    r.m.focusNext(1);
    CHECK(r.m.focused() == 0);
    r.m.focusNext(1);
    CHECK(r.m.focused() == 1);  // wraps
    r.m.focusNext(-1);
    CHECK(r.m.focused() == 0);
    r.m.setSelection({1, 2});
    const auto n = r.m.node(2);
    CHECK(r.m.contextTargets(n.x, n.y) == std::vector<int>{1, 2});  // inside the selection
    const auto o = r.m.node(0);
    CHECK(r.m.contextTargets(o.x, o.y) == std::vector<int>{0});  // outside: becomes the selection
}
