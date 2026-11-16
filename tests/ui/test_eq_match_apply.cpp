// EQ Match apply (area 16 R10-R14): one undo step including slider moves, source EQ Match, the
// Bands prefix and Amount scaling, tagged bands replaced on re-apply, and a partial apply with a
// message when free bands run out.
#include "amq/params/mappings.hpp"
#include "amq/ui/band_ops.hpp"
#include "amq/ui/eq_match_apply.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <map>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {
class Host : public ui::EditController {
public:
    params::ParameterTable t;
    ParamSource source = ParamSource::Gui;
    int eqMatchWrites = 0;
    std::map<params::ParamIndex, int> open;
    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override { ++open[i]; }
    void performEdit(params::ParamIndex i, double v) override {
        CHECK(open[i] > 0);
        eqMatchWrites += source == ParamSource::EqMatch;
        t.set(i, v, source);
    }
    void endEdit(params::ParamIndex i) override { --open[i]; }
    void setEditSource(ParamSource s) override { source = s; }
};
double v(const Host& h, int b, BandField f) { return h.t.value(params::bandParam(b, f)); }
const std::vector<ui::MatchBand> kFit = {{dsp::Shape::Bell, dsp::Slope::Db12, 1000.0, 6.0, 2.0},
                                         {dsp::Shape::LowCut, dsp::Slope::Db48, 40.0, 0.0, 0.7071},
                                         {dsp::Shape::HighShelf, dsp::Slope::Db12, 8000.0, -4.0, 0.7071}};
}  // namespace

TEST_CASE("Apply: one undo step across slider moves, source EQ Match, prefix and Amount") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ops.create(300, 2);  // a user band stays untouched
    const size_t steps = u.undoSteps();
    ui::EqMatchApplier a(u);
    a.setFit(kFit);
    std::string msg;
    CHECK(a.apply(1.0, 3, &msg) == 3);
    CHECK(msg.empty());
    CHECK(h.eqMatchWrites > 0);
    CHECK(a.taggedBands() == std::vector<int>{1, 2, 3});
    CHECK(v(h, 1, BandField::Gain) == 6.0);
    CHECK(v(h, 2, BandField::Slope) == static_cast<int>(dsp::Slope::Db48));
    CHECK(params::qFromPosition(v(h, 1, BandField::Q)) == doctest::Approx(2.0).epsilon(1e-6));
    // Slider moves: half the Amount, two bands.
    CHECK(a.apply(0.5, 2, &msg) == 2);
    CHECK(v(h, 1, BandField::Gain) == 3.0);
    CHECK(v(h, 2, BandField::Slope) < static_cast<int>(dsp::Slope::Db48));  // softer cut
    CHECK(v(h, 3, BandField::Used) == 0.0);  // the third band went away
    CHECK(a.apply(0.05, 2, &msg) == 1);       // below the bypass Amount the cut drops out
    a.finish();
    CHECK(h.source == ParamSource::Gui);
    CHECK(u.undoSteps() == steps + 1);  // everything above is one step (R12, R13)
    CHECK(u.undoLabel() == "EQ Match");
    u.undo();
    for (int b = 1; b < 4; ++b) CHECK(v(h, b, BandField::Used) == 0.0);
    CHECK(v(h, 0, BandField::Gain) == 2.0);
}

TEST_CASE("Apply: a re-apply replaces the tagged bands; too few free bands apply the strongest with a message") {
    Host h;
    ui::UndoController u(h);
    ui::BandOps ops(u);
    ui::EqMatchApplier a(u);
    a.setFit(kFit);
    std::string msg;
    a.apply(1.0, 3, &msg);
    a.setFit({{dsp::Shape::Bell, dsp::Slope::Db12, 2000.0, -2.0, 1.0}});
    CHECK(a.apply(1.0, 1, &msg) == 1);
    a.finish();
    CHECK(ops.usedBands() == std::vector<int>{0});  // replaced, not added (re-apply)
    CHECK(std::exp2(v(h, 0, BandField::Freq)) == doctest::Approx(2000.0).epsilon(1e-6));
    while (ops.freeBands() > 1) ops.create(500, 1);  // one free band left, plus the tagged one
    a.setFit(kFit);
    CHECK(a.apply(1.0, 3, &msg) == 2);
    CHECK(msg == "1 fitted band not applied: no free bands.");
    a.finish();
}
