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
