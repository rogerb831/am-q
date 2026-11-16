// Undo history, band-operations service and menu model (area 12 undo task; area 14 R2-R7, R10;
// area 21 R5, R7, R8).
#include "amq/ui/band_menu.hpp"
#include "amq/ui/band_ops.hpp"
#include "amq/ui/edit_gesture.hpp"
#include "amq/ui/undo_controller.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

class Host : public ui::EditController {
public:
    params::ParameterTable t;
    int begins = 0, performs = 0, ends = 0;
    std::map<params::ParamIndex, int> open;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override { ++begins, ++open[i]; }
    void performEdit(params::ParamIndex i, double v) override {
        CHECK(open[i] > 0);
        ++performs;
        t.set(i, v, ParamSource::Gui);
    }
    void endEdit(params::ParamIndex i) override { ++ends, --open[i]; }
    bool balanced() const {
        for (const auto& [i, n] : open)
            if (n) return false;
        return begins == ends;
    }
};

double v(const Host& h, int b, BandField f) { return h.t.value(params::bandParam(b, f)); }
bool allDefault(const Host& h, int b) {
    for (int f = 0; f < params::kBandFieldCount; ++f) {
        const auto i = params::bandParam(b, static_cast<BandField>(f));
        if (h.t.value(i) != params::specAt(i)->def) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("Undo: one gesture is one step; undo and redo go through host gestures") {
    Host h;
    ui::UndoController u(h);
    const auto g = params::bandParam(0, BandField::Gain);
    {
        ui::EditGesture e(u, {g});
        e.set(g, 3.0);
        e.set(g, 6.0);
    }
    CHECK(u.undoSteps() == 1);
    CHECK(h.t.value(g) == 6.0);
    CHECK(u.undo());
    CHECK(h.t.value(g) == 0.0);
    CHECK(u.canRedo());
    CHECK(u.redo());
    CHECK(h.t.value(g) == 6.0);
    CHECK(h.balanced());
    // A step that changes nothing is dropped; a new step clears redo.
    {
        ui::EditGesture e(u, {g});
        e.set(g, 6.0);
    }
    CHECK(u.undoSteps() == 1);
    u.undo();
    {
        ui::EditGesture e(u, {g});
        e.set(g, -2.0);
    }
    CHECK_FALSE(u.canRedo());
}

TEST_CASE("Undo depth bounds the history; clear empties it") {
    Host h;
    ui::UndoController u(h);
    u.setDepth(5);
    const auto g = params::bandParam(0, BandField::Gain);
    for (int k = 1; k <= 8; ++k) {
        ui::EditGesture e(u, {g});
        e.set(g, k);
    }
    CHECK(u.undoSteps() == 5);
    int n = 0;
    while (u.undo()) ++n;
    CHECK(n == 5);
    CHECK(h.t.value(g) == 3.0);  // the oldest three fell off
    u.clear();
    CHECK_FALSE(u.canUndo());
    CHECK_FALSE(u.canRedo());
}

TEST_CASE("Create: every default plus the gesture's values in one undo step (area 21 R8)") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    const int b = ops.create(250.0, -4.0);
    CHECK(b == 0);
    CHECK(u.undoSteps() == 1);
    CHECK(u.undoLabel() == "Create Band");
    CHECK(v(h, 0, BandField::Used) == 1.0);
    CHECK(v(h, 0, BandField::Gain) == -4.0);
    CHECK(std::exp2(v(h, 0, BandField::Freq)) == doctest::Approx(250.0));
    CHECK(v(h, 0, BandField::ScLow) == params::specAt(params::bandParam(0, BandField::ScLow))->def);  // SC open
    CHECK(v(h, 0, BandField::SpectralTilt) == 0.0);
    u.undo();
    CHECK(allDefault(h, 0));
    CHECK(h.balanced());
}

TEST_CASE("Delete: Used off and every field back to default, one step; other bands untouched (R6, R7)") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    for (int k = 0; k < 4; ++k) ops.create(100.0 * (k + 1), k + 1.0);
    ops.makeDynamic({1, 2});
    const size_t steps = u.undoSteps();
    ops.remove(ui::BandOps::targets({1, 2}, 3));
    CHECK(u.undoSteps() == steps + 1);
    CHECK(allDefault(h, 1));
    CHECK(allDefault(h, 2));
    CHECK(v(h, 0, BandField::Used) == 1.0);
    CHECK(v(h, 3, BandField::Gain) == 4.0);
    // Nothing selected: the focused band.
    CHECK(ui::BandOps::targets({}, 3) == ui::BandOps::Bands{3});
    CHECK(ui::BandOps::targets({}, -1).empty());
    u.undo();
    CHECK(v(h, 1, BandField::DynEnabled) == 1.0);
    CHECK(v(h, 2, BandField::Gain) == 3.0);
}

TEST_CASE("Operations: enable, invert, dynamics with a default range, spectral, shape, slope, placement") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(1000, 5);
    ops.create(4000, -2);
    ops.setEnabled({0, 1}, false);
    CHECK(ops.disabledBands().size() == 2);
    ops.invertGain({0, 1});
    CHECK(v(h, 0, BandField::Gain) == -5.0);
    CHECK(v(h, 1, BandField::Gain) == 2.0);
    ops.makeDynamic({0});
    CHECK(v(h, 0, BandField::DynEnabled) == 1.0);
    CHECK(v(h, 0, BandField::DynRange) == ui::kDefaultDynamicRangeDb);  // hearable at once (R5)
    ops.makeSpectral({1});
    CHECK(v(h, 1, BandField::Spectral) == 1.0);
    CHECK(v(h, 1, BandField::DynEnabled) == 1.0);
    const size_t before = u.undoSteps();
    ops.clearDynamics({0, 1});
    CHECK(u.undoSteps() == before + 1);
    CHECK(v(h, 0, BandField::DynEnabled) == 0.0);
    CHECK(v(h, 0, BandField::DynRange) == 0.0);
    CHECK(v(h, 1, BandField::Spectral) == 0.0);
    ops.setShape({0}, dsp::Shape::HighCut);
    ops.setSlope({0, 1}, dsp::Slope::Brickwall);  // only the cut takes Brickwall
    CHECK(v(h, 0, BandField::Slope) == static_cast<int>(dsp::Slope::Brickwall));
    CHECK(v(h, 1, BandField::Slope) != static_cast<int>(dsp::Slope::Brickwall));
    ops.setShape({0}, dsp::Shape::Bell);  // Brickwall is not allowed for a Bell: the slope resets
    CHECK(dsp::slopeAllowed(dsp::Shape::Bell, static_cast<dsp::Slope>(static_cast<int>(v(h, 0, BandField::Slope)))));
    ops.setPlacement({0, 1}, 3);
    CHECK(v(h, 1, BandField::Placement) == 3.0);
    CHECK(h.balanced());
}

TEST_CASE("Clipboard: copy and paste round trip, cut is one step, a full set refuses with a message (R10)") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(300, 2.5);
    ops.create(3000, -6);
    ops.makeDynamic({1});
    const std::string clip = ops.copy({0, 1});
    ui::BandOps::Bands made;
    std::string msg;
    CHECK(ops.paste(clip, {}, &msg, &made));
    CHECK(made == ui::BandOps::Bands{2, 3});
    for (int f = 0; f < params::kBandFieldCount; ++f) {
        CHECK(v(h, 2, static_cast<BandField>(f)) == v(h, 0, static_cast<BandField>(f)));
        CHECK(v(h, 3, static_cast<BandField>(f)) == v(h, 1, static_cast<BandField>(f)));
    }
    const size_t steps = u.undoSteps();
    const std::string c2 = ops.cut({2, 3});
    CHECK(u.undoSteps() == steps + 1);
    CHECK(c2 == clip);
    CHECK(allDefault(h, 2));
    // Fill the set, then a paste needing two bands fails and changes nothing.
    while (ops.freeBands() > 1) ops.create(1000, 1);
    const size_t s2 = u.undoSteps();
    CHECK_FALSE(ops.paste(clip, {}, &msg));
    CHECK(msg == "Not enough free bands: 2 needed, 1 free.");
    CHECK(u.undoSteps() == s2);
    // Pasting over the targets reuses delete: room for both.
    CHECK(ops.paste(clip, {5, 6}, &msg));
    CHECK_FALSE(ops.paste("not ours", {}, &msg));
    CHECK(msg == "The clipboard holds no Am-Q bands.");
}

TEST_CASE("Menu model: labels follow the targets' state; actions are one step each (R2, R4)") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(500, 3);
    ops.create(5000, -3);
    ops.create(80, 0, dsp::Shape::LowCut);
    auto find = [](const std::vector<ui::MenuItem>& m, int a) {
        return std::find_if(m.begin(), m.end(), [&](const ui::MenuItem& i) { return i.action == a; });
    };
    auto m = ui::bandMenu(ops, {0, 1}, false);
    CHECK(find(m, ui::menu::Disable) != m.end());  // all enabled: offer Disable
    CHECK(find(m, ui::menu::Delete)->label == "Delete 2 Bands");
    CHECK_FALSE(find(m, ui::menu::Paste)->enabled);
    CHECK_FALSE(find(m, ui::menu::Split)->enabled);
    CHECK(find(m, ui::menu::ShapeBase + static_cast<int>(dsp::Shape::Bell))->checked);
    auto one = ui::bandMenu(ops, {2}, true);
    CHECK(find(one, ui::menu::Delete)->label == "Delete Band");
    CHECK_FALSE(find(one, ui::menu::MakeDynamic)->enabled);  // a cut has no gain
    CHECK(find(one, ui::menu::SlopeBase + static_cast<int>(dsp::Slope::Brickwall))->enabled);
    std::string clip, msg;
    const size_t steps = u.undoSteps();
    CHECK(ui::activate(ops, ui::menu::Disable, {0, 1}, clip, &msg));
    CHECK(u.undoSteps() == steps + 1);
    m = ui::bandMenu(ops, {0, 1}, false);
    CHECK(find(m, ui::menu::Enable) != m.end());
    CHECK(ui::activate(ops, ui::menu::DeleteDisabled, {}, clip, &msg));
    CHECK(ops.usedBands() == ui::BandOps::Bands{2});
    CHECK_FALSE(ui::activate(ops, ui::menu::SelectAll, {}, clip, &msg));  // the editor's job
    auto g = ui::graphMenu(ops, false);
    CHECK(find(g, ui::menu::DeleteAll)->enabled);
    CHECK_FALSE(find(g, ui::menu::Paste)->enabled);
}
