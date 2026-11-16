// Key dispatcher (area 15 R1-R6, area 14 R9, area 21 R9) and band panel model (area 12 panel tasks,
// area 13 binding, area 15 R7-R9, area 06 R6 and the effective-time tooltip; area 11 menus).
#include "amq/dsp/dynamics.hpp"
#include "amq/params/mappings.hpp"
#include "amq/params/shape_info.hpp"
#include "amq/ui/band_menu.hpp"
#include "amq/ui/band_panel.hpp"
#include "amq/ui/key_dispatcher.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

class Host : public ui::EditController {
public:
    params::ParameterTable t;
    std::map<params::ParamIndex, int> open;
    std::vector<ParamSource> sources;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override { ++open[i]; }
    void performEdit(params::ParamIndex i, double v) override {
        CHECK(open[i] > 0);  // every write inside a host transaction (area 21 R5)
        t.set(i, v, ParamSource::Gui);
    }
    void endEdit(params::ParamIndex i) override { --open[i]; }
};

struct Rig {
    Host h;
    ui::UndoController u{h};
    ui::BandOps ops{u};
    ui::GraphGeometry g;
    ui::GraphInteraction m{u, g};
    ui::KeyDispatcher keys{m, ops};
    Rig() { g.setSize(1000.0f, 600.0f); }
    double v(int b, BandField f) const { return h.t.value(params::bandParam(b, f)); }
    bool key(ui::Key k, ui::Modifiers mods = {}, char32_t ch = 0) { return keys.keyDown({k, ch, mods}); }
};

const ui::Modifiers kShift{true, false, false}, kCmd{false, false, true}, kCmdShift{true, false, true};

}  // namespace

TEST_CASE("Keys: Delete and Backspace delete the selected bands, with bands actually selected (area 21 R9)") {
    Rig r;
    r.ops.create(100, 1), r.ops.create(1000, 2), r.ops.create(5000, 3);
    r.m.setSelection({0, 2});
    CHECK(r.key(ui::Key::Delete));
    CHECK(r.ops.usedBands() == std::vector<int>{1});
    CHECK(r.m.selection().empty());
    r.m.setSelection({1}, 1);
    CHECK(r.key(ui::Key::Backspace));
    CHECK(r.ops.usedBands().empty());
    CHECK_FALSE(r.key(ui::Key::Delete));  // nothing to delete: the host gets the key
    CHECK(r.key(ui::Key::Character, kCmd, U'z'));  // undo brings band 1 back
    CHECK(r.ops.usedBands() == std::vector<int>{1});
    CHECK(r.key(ui::Key::Character, kCmdShift, U'z'));  // redo
    CHECK(r.ops.usedBands().empty());
}

TEST_CASE("Keys: arrows nudge (Shift fine), each press one undo step; Tab moves focus; Escape clears") {
    Rig r;
    r.ops.create(1000, 0);
    r.ops.create(200, 0, dsp::Shape::LowCut);
    r.m.setSelection({0}, 0);
    const double f0 = r.v(0, BandField::Freq);
    const size_t steps = r.u.undoSteps();
    CHECK(r.key(ui::Key::Right));
    CHECK(r.v(0, BandField::Freq) == doctest::Approx(f0 + 1.0 / 12.0));
    CHECK(r.key(ui::Key::Left, kShift));
    CHECK(r.v(0, BandField::Freq) == doctest::Approx(f0 + 1.0 / 12.0 - 1.0 / 96.0));
    CHECK(r.key(ui::Key::Up));
    CHECK(r.v(0, BandField::Gain) == doctest::Approx(0.5));
    CHECK(r.u.undoSteps() == steps + 3);
    r.m.setSelection({1}, 1);  // a cut: Up moves Q
    const double q = r.v(1, BandField::Q);
    CHECK(r.key(ui::Key::Up));
    CHECK(r.v(1, BandField::Q) == doctest::Approx(q + ui::KeyDispatcher::kNudgeQ));
    CHECK(r.key(ui::Key::Tab));
    CHECK(r.m.focused() == 0);  // by frequency: 200 Hz (band 1), then 1 kHz (band 0)
    CHECK(r.key(ui::Key::Escape));
    CHECK(r.m.selection().empty());
    CHECK_FALSE(r.key(ui::Key::Escape));
    CHECK_FALSE(r.key(ui::Key::Character, {}, U' '));  // Space passes through to the host
    CHECK_FALSE(r.key(ui::Key::Other));
}

TEST_CASE("Keys: Command+A selects all; C, X, V use the band clipboard and a visible message when full") {
    Rig r;
    r.ops.create(100, 1), r.ops.create(1000, 2);
    std::string system;
    r.keys.writeSystemClipboard = [&](const std::string& s) { system = s; };
    r.keys.readSystemClipboard = [&] { return system; };
    CHECK(r.key(ui::Key::Character, kCmd, U'a'));
    CHECK(r.m.selection() == std::vector<int>{0, 1});
    CHECK(r.key(ui::Key::Character, kCmd, U'c'));
    CHECK(system.find("am-q.bands") != std::string::npos);
    CHECK(r.key(ui::Key::Character, kCmd, U'v'));
    CHECK(r.ops.usedBands().size() == 4);
    CHECK(r.m.selection() == std::vector<int>{2, 3});  // the pasted bands become the selection
    while (r.ops.freeBands() > 0) r.ops.create(500, 0);
    CHECK(r.key(ui::Key::Character, kCmd, U'v'));
    CHECK(r.keys.message == "Not enough free bands: 2 needed, 0 free.");
    r.m.setSelection({0});
    CHECK(r.key(ui::Key::Character, kCmd, U'x'));
    CHECK_FALSE(r.ops.used(0));
}

TEST_CASE("Band panel: visibility by shape keeps hidden values; Auto greys timing; tooltips show ms") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(1000, 6);
    auto find = [](const std::vector<ui::PanelControl>& c, const std::string& l) {
        return *std::find_if(c.begin(), c.end(), [&](const ui::PanelControl& p) { return p.label == l; });
    };
    auto p = ui::bandPanel(h.t, 0);
    CHECK(find(p, "Gain").visible);
    CHECK(find(p, "Slope").visible);  // Bells have slopes (minimum order 2, area 01 R10)
    CHECK(find(p, "Gain").text == "+6.00 dB");
    CHECK_FALSE(find(p, "Range").enabled);  // dynamics off
    ops.setShape({0}, dsp::Shape::LowCut);
    p = ui::bandPanel(h.t, 0);
    CHECK_FALSE(find(p, "Gain").visible);
    CHECK(find(p, "Slope").visible);
    CHECK_FALSE(find(p, "Dynamic").visible);
    CHECK(h.t.value(params::bandParam(0, BandField::Gain)) == 6.0);  // the hidden gain is kept
    ops.setShape({0}, dsp::Shape::FlatTilt);  // ignores Q and slope
    p = ui::bandPanel(h.t, 0);
    CHECK_FALSE(find(p, "Q").visible);
    CHECK_FALSE(find(p, "Slope").visible);
    CHECK(find(p, "Gain").visible);
    ops.setShape({0}, dsp::Shape::Bell);
    ops.makeDynamic({0});
    p = ui::bandPanel(h.t, 0);
    CHECK(find(p, "Range").enabled);
    CHECK_FALSE(find(p, "Attack").enabled);  // Auto on: greyed (R6)
    CHECK(find(p, "Attack").tooltip.find("ms") != std::string::npos);
    CHECK_FALSE(find(p, "Low").visible);  // Band mode
    h.t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Preset);
    h.t.set(params::bandParam(0, BandField::ScMode), 1, ParamSource::Preset);
    p = ui::bandPanel(h.t, 0);
    CHECK(find(p, "Attack").enabled);
    CHECK(find(p, "Low").visible);
    CHECK_FALSE(find(p, "Density").enabled);  // Spectral off
    CHECK(find(p, "Clear Dynamics").enabled);
    CHECK(ui::bandPanel(h.t, -1).empty());
}

TEST_CASE("Choice items come from the table; the Shape menu uses ShapeInfo; one undo step per change") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(1000, 0);
    const auto shapes = ui::choiceItems(h.t, params::bandParam(0, BandField::Shape));
    REQUIRE(shapes.size() == static_cast<size_t>(dsp::kShapeCount));
    for (const auto& it : shapes) CHECK(it.label == params::shapeInfo(static_cast<dsp::Shape>(it.value)).name);
    CHECK(shapes[0].checked);
    const auto slopes = ui::choiceItems(h.t, params::bandParam(0, BandField::Slope));
    CHECK_FALSE(slopes.back().enabled);  // Brickwall is for cuts only
    const auto& spec = *params::specAt(params::bandParam(0, BandField::Placement));
    const auto places = ui::choiceItems(h.t, params::bandParam(0, BandField::Placement));
    for (size_t i = 0; i < places.size(); ++i) CHECK(places[i].label == spec.choices[i]);
    const size_t steps = u.undoSteps();
    ops.setShape({0}, dsp::Shape::HighShelf);
    CHECK(u.undoSteps() == steps + 1);
    // The band menu reads the same labels.
    const auto m = ui::bandMenu(ops, {0}, false);
    CHECK(std::any_of(m.begin(), m.end(), [&](const ui::MenuItem& i) { return i.label == spec.choices[3]; }));
}
