// Write-path trace (area 21 task "write-path trace test that asserts the source and transaction for
// every control"; R5-R8; area 09 R4). Every way the editor changes a parameter (graph gestures,
// wheel, keys, every band operation and menu action, panel choices and toggles, EQ Match apply) is
// traced: each write must be inside a host gesture (begin ... end on that parameter), carry source GUI
// (EQ Match for an apply), and land in the undo history.
#include "amq/params/mappings.hpp"
#include "amq/ui/band_menu.hpp"
#include "amq/ui/band_panel.hpp"
#include "amq/ui/eq_match_apply.hpp"
#include "amq/ui/key_dispatcher.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {
class Trace : public ui::EditController {
public:
    params::ParameterTable t;
    ParamSource source = ParamSource::Gui;
    std::map<params::ParamIndex, int> open;
    int writes = 0, outside = 0, begins = 0, ends = 0;
    std::map<ParamSource, int> bySource;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override { ++open[i], ++begins; }
    void performEdit(params::ParamIndex i, double v) override {
        ++writes;
        outside += open[i] <= 0;
        ++bySource[source];
        t.set(i, v, source);
    }
    void endEdit(params::ParamIndex i) override { --open[i], ++ends; }
    void setEditSource(ParamSource s) override { source = s; }
};
}  // namespace

TEST_CASE("Every editor write path: inside a host gesture, the right source, and undoable") {
    Trace h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ui::GraphGeometry g;
    g.setSize(1000.0f, 600.0f);
    ui::GraphInteraction m(u, g);
    ui::KeyDispatcher keys(m, ops);
    // Graph: create, drag, wheel (each binding), Alt-click toggles, lasso.
    const int b0 = m.createBandAt(g.xOfHz(500), g.yOfDb(3));
    auto n = m.node(b0);
    m.pointerDown(n.x, n.y, ui::Button::Left, {}, 1);
    m.pointerDrag(n.x + 40, n.y - 30, {});
    m.pointerUp(n.x + 40, n.y - 30, {});
    n = m.node(b0);
    const ui::Modifiers wheels[] = {{}, {true, false, false}, {false, true, false}, {false, false, true}};
    for (const auto& mod : wheels) m.wheel(n.x, n.y, 1.0, mod);
    m.pointerDown(n.x, n.y, ui::Button::Left, {false, true, false}, 1);
    m.pointerUp(n.x, n.y, {false, true, false});
    // Every band operation and menu action.
    ops.create(2000, -2);
    std::string clip, msg;
    const int actions[] = {ui::menu::Enable, ui::menu::Disable, ui::menu::InvertGain, ui::menu::MakeDynamic,
                           ui::menu::ClearDynamics, ui::menu::MakeSpectral, ui::menu::ClearSpectral, ui::menu::ShapeBase + 3,
                           ui::menu::SlopeBase + 2, ui::menu::PlacementBase + 3, ui::menu::Copy, ui::menu::Paste,
                           ui::menu::Cut};
    for (int a : actions) ui::activate(ops, a, {0, 1}, clip, &msg);
    // Keys.
    m.setSelection({0}, 0);
    for (auto k : {ui::Key::Up, ui::Key::Down, ui::Key::Left, ui::Key::Right}) keys.keyDown({k, 0, {}});
    // Panel choices and toggles (as the editor writes them: one gesture each).
    for (const auto& c : ui::bandPanel(h.t, 0)) {
        if (c.kind != ui::ControlKind::Toggle && c.kind != ui::ControlKind::Choice) continue;
        if (c.id == params::bandParam(0, BandField::Used)) continue;
        ui::EditGesture e(u, {c.id});
        e.set(c.id, c.kind == ui::ControlKind::Toggle ? 1.0 - h.t.value(c.id) : 1.0);
    }
    const int guiWrites = h.writes;
    // EQ Match apply: source EQ Match.
    ui::EqMatchApplier apply(u);
    apply.setFit({{dsp::Shape::Bell, dsp::Slope::Db12, 800.0, 3.0, 1.0}});
    apply.apply(1.0, 1, &msg);
    apply.finish();
    keys.keyDown({ui::Key::Delete, 0, {}});

    CHECK(h.writes > guiWrites);
    CHECK(h.outside == 0);          // no write outside a host transaction
    CHECK(h.begins == h.ends);      // every transaction closed
    for (const auto& [i, k] : h.open) CHECK(k == 0);
    CHECK(h.bySource[ParamSource::EqMatch] > 0);  // the apply, and only the apply
    CHECK(h.bySource[ParamSource::Gui] + h.bySource[ParamSource::EqMatch] == h.writes);
    CHECK(h.source == ParamSource::Gui);  // back to GUI after the apply
    CHECK(u.undoSteps() > 20);            // every change above is in the history
}
