// Accessible focus (area 15 R3, R12): the keys and assistive tools share one focus, the graph's
// focused band, and both move through the used bands in frequency order.
#include "amq/params/mappings.hpp"
#include "amq/ui/graph_accessible.hpp"
#include "amq/ui/key_dispatcher.hpp"

#include <doctest/doctest.h>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {
class Host : public ui::EditController {
public:
    params::ParameterTable t;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex) override {}
    void performEdit(params::ParamIndex i, double v) override { t.set(i, v, ParamSource::Gui); }
    void endEdit(params::ParamIndex) override {}
};
void band(Host& h, int b, double hz) {
    h.t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    h.t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
}
}  // namespace

TEST_CASE("Accessible focus: one focus shared with Tab, in frequency order") {
    Host h;
    ui::UndoController u(h);
    ui::GraphGeometry g;
    g.setSize(1180.0f, 584.0f);
    ui::GraphInteraction m(u, g);
    ui::BandOps ops(u);
    ui::KeyDispatcher keys(m, ops);
    band(h, 4, 5000.0);
    band(h, 1, 100.0);
    band(h, 7, 1000.0);  // band 7 sits between bands 1 and 4 by frequency
    ui::GraphAccessible graph(u, m);

    // The tree lists the used bands only, in frequency order.
    const auto kids = graph.accessibleChildren();
    REQUIRE(kids.size() == 3);
    CHECK(kids[0]->accessibleName() == "Band 2");
    CHECK(kids[1]->accessibleName() == "Band 8");
    CHECK(kids[2]->accessibleName() == "Band 5");
    CHECK_FALSE(graph.accessibleFocusable());  // groups are not focusable themselves
    CHECK(ui::accessibleFocusedControl(graph) == nullptr);

    // An assistive tool moves the focus: the graph's focused band follows.
    auto* c = ui::accessibleFocusNext(graph, +1);
    REQUIRE(c != nullptr);
    CHECK(c->accessibleName() == "Band 2");
    CHECK(m.focused() == 1);

    // Tab moves the same focus, and the tree reports it.
    CHECK(keys.keyDown({ui::Key::Tab, 0, {}}));
    CHECK(m.focused() == 7);
    CHECK(ui::accessibleFocusedControl(graph)->accessibleName() == "Band 8");
    c = ui::accessibleFocusNext(graph, +1);
    CHECK(c->accessibleName() == "Band 5");
    CHECK(m.focused() == 4);
    c = ui::accessibleFocusNext(graph, +1);  // wraps
    CHECK(m.focused() == 1);
    c = ui::accessibleFocusNext(graph, -1);  // and back
    CHECK(m.focused() == 4);

    // A click (selection) is the same focus too.
    m.setSelection({7}, 7);
    CHECK(ui::accessibleFocusedControl(graph)->accessibleName() == "Band 8");

    // Taking focus needs a visible control: a deleted band can't.
    ui::BandNodeAccessible gone(u, m, 10);
    CHECK_FALSE(gone.setAccessibleFocus());
}

#include "amq/ui/editor_accessible.hpp"

TEST_CASE("Editor tree: every region's controls, with one shared focus") {
    Host h;
    ui::UndoController u(h);
    ui::GraphGeometry g;
    g.setSize(1180.0f, 584.0f);
    ui::GraphInteraction m(u, g);
    ui::BandOps ops(u);
    band(h, 2, 300.0);
    band(h, 5, 3000.0);
    ui::EditorAccessible tree(u, m, ops);
    int undos = 0;
    tree.setCommand("Undo", [&] { ++undos; }, [] { return true; });

    auto& root = tree.root();
    const auto regions = root.accessibleChildren();
    REQUIRE(regions.size() == 4);
    CHECK(regions[0]->accessibleName() == "Top bar");
    CHECK(regions[1]->accessibleName() == "Response graph");
    CHECK(regions[2]->accessibleName() == "Band panel");
    CHECK(regions[3]->accessibleName() == "Status bar");
    CHECK(regions[2]->accessibleChildren().empty());  // no band focused: no panel

    // A command runs; a disabled command is not focusable.
    auto* undo = regions[0]->accessibleChildren()[0];
    CHECK(undo->accessibleName() == "Undo");
    CHECK(undo->performAccessibleAction(ui::AccessibleAction::Press));
    CHECK(undos == 1);
    CHECK_FALSE(regions[0]->accessibleChildren()[1]->accessibleFocusable());  // Redo: not bound

    // Focus a band: the panel lists its controls, named after the band.
    m.setSelection({5}, 5);
    const auto panel = regions[2]->accessibleChildren();
    REQUIRE_FALSE(panel.empty());
    auto byName = [&](const std::string& n) -> ui::AccessibleControl* {
        for (auto* c : panel)
            if (c->accessibleName() == n) return c;
        return nullptr;
    };
    auto* gain = byName("Band 6 Gain");
    REQUIRE(gain != nullptr);
    CHECK(gain->accessibleRole() == ui::AccessibleRole::Slider);
    CHECK(byName("Band 6 Delete") == nullptr);
    CHECK(byName("Delete band") != nullptr);
    // Threshold is greyed until the band is dynamic and Auto is off.
    auto* thr = byName("Band 6 Threshold");
    REQUIRE(thr != nullptr);
    CHECK_FALSE(thr->accessibleEnabled());

    // Values change through gestures; the tree reports the formatted value.
    CHECK(gain->performAccessibleAction(ui::AccessibleAction::SetValue, 0.75));
    CHECK(h.t.value(params::bandParam(5, BandField::Gain)) == doctest::Approx(15.0));
    CHECK(gain->accessibleValueText() == "+15.0 dB");
    auto* dyn = byName("Band 6 Dynamic");
    REQUIRE(dyn != nullptr);
    CHECK(dyn->accessibleRole() == ui::AccessibleRole::Toggle);
    CHECK(dyn->performAccessibleAction(ui::AccessibleAction::Press));
    CHECK(h.t.value(params::bandParam(5, BandField::DynEnabled)) == 1.0);

    // One focus: a control takes it from the band node; the band node takes it back.
    CHECK(ui::accessibleFocusedControl(root)->accessibleName() == "Band 6");
    CHECK(gain->setAccessibleFocus());
    CHECK(ui::accessibleFocusedControl(root) == gain);
    CHECK(tree.focusedParam() == static_cast<long>(params::bandParam(5, BandField::Gain)));
    auto* next = ui::accessibleFocusNext(root, +1);
    REQUIRE(next != nullptr);
    CHECK(next != gain);
    CHECK(regions[1]->accessibleChildren()[0]->setAccessibleFocus());  // Band 3 (300 Hz)
    CHECK(m.focused() == 2);
    CHECK(ui::accessibleFocusedControl(root)->accessibleName() == "Band 3");
    CHECK(tree.focusedParam() == -1);

    // The status bar's globals.
    const auto status = regions[3]->accessibleChildren();
    CHECK(status.front()->accessibleName() == "Processing Mode");
    CHECK(status.front()->accessibleRole() == ui::AccessibleRole::Choice);
    CHECK(status.front()->performAccessibleAction(ui::AccessibleAction::Press));  // next mode
    CHECK(h.t.value(params::globalParam(params::GlobalParam::Mode)) == 1.0);
}

TEST_CASE("Panel keys: Enter goes in, Tab moves, Up and Down adjust, Enter presses, Escape goes back") {
    Host h;
    ui::UndoController u(h);
    ui::GraphGeometry g;
    g.setSize(1180.0f, 584.0f);
    ui::GraphInteraction m(u, g);
    ui::BandOps ops(u);
    band(h, 3, 1000.0);
    ui::EditorAccessible tree(u, m, ops);
    using PK = ui::EditorAccessible::PanelKey;

    CHECK_FALSE(tree.panelKey(PK::Enter, false));  // no focused band: nothing to enter
    m.setSelection({3}, 3);
    CHECK_FALSE(tree.panelHasFocus());
    CHECK_FALSE(tree.panelKey(PK::Up, false));     // the graph keeps Up (gain nudge)
    REQUIRE(tree.panelKey(PK::Enter, false));      // into the panel: its first control
    CHECK(tree.panelHasFocus());
    const std::string first = tree.focusedName();
    CHECK(first.rfind("Band 4 ", 0) == 0);

    // Tab to Gain, then adjust it.
    for (int i = 0; i < 30 && tree.focusedName() != "Band 4 Gain"; ++i) tree.panelKey(PK::Next, false);
    REQUIRE(tree.focusedName() == "Band 4 Gain");
    const auto gid = params::bandParam(3, BandField::Gain);
    CHECK(tree.panelKey(PK::Up, false));
    CHECK(h.t.value(gid) == doctest::Approx(0.6));   // 1 % of the 60 dB range
    CHECK(tree.panelKey(PK::Down, true));            // fine: 0.1 %
    CHECK(h.t.value(gid) == doctest::Approx(0.54));
    CHECK(tree.panelKey(PK::Previous, false));
    CHECK(tree.focusedName() != "Band 4 Gain");

    // Enter presses a toggle.
    for (int i = 0; i < 30 && tree.focusedName() != "Band 4 Enabled"; ++i) tree.panelKey(PK::Next, false);
    REQUIRE(tree.focusedName() == "Band 4 Enabled");
    CHECK(tree.panelKey(PK::Enter, false));
    CHECK(h.t.value(params::bandParam(3, BandField::Enabled)) == 0.0);

    // Escape: back to the band node.
    CHECK(tree.panelKey(PK::Escape, false));
    CHECK_FALSE(tree.panelHasFocus());
    CHECK(ui::accessibleFocusedControl(tree.root())->accessibleName() == "Band 4");
}
