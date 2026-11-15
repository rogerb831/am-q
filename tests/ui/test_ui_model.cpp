// Editor model tests without a window (area 09 R4, area 10 R6, area 12 R12, area 13 R1-R3,
// R7, R9, R15, R18, R19, R22; area 15 R12 is covered through the plugin GUI test).
#include "amq/dsp/analog_prototype.hpp"
#include "amq/params/mappings.hpp"
#include "amq/ui/graph_model.hpp"
#include "amq/ui/param_attachment.hpp"
#include "amq/ui/snapshot_poller.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

// Records the host-facing calls and writes the table like the plugin does.
class MockController : public ui::EditController {
public:
    params::ParameterTable t;
    std::vector<std::string> log;  // "B id", "P id", "E id"
    std::map<params::ParamIndex, int> open;

    const params::ParameterTable& table() const noexcept override { return t; }
    void addListener(params::ParameterListener* l) override { t.addListener(l); }
    void removeListener(params::ParameterListener* l) override { t.removeListener(l); }
    void dispatchNotifications() override { t.dispatchNotifications(); }
    void beginEdit(params::ParamIndex i) override {
        log.push_back("B " + params::specAt(i)->id);
        ++open[i];
    }
    void performEdit(params::ParamIndex i, double v) override {
        CHECK(open[i] > 0);  // performs only inside a gesture (area 13 R18)
        log.push_back("P " + params::specAt(i)->id);
        t.set(i, v, ParamSource::Gui);
    }
    void endEdit(params::ParamIndex i) override {
        log.push_back("E " + params::specAt(i)->id);
        --open[i];
    }
    int count(char kind) const {
        int n = 0;
        for (const auto& s : log) n += s[0] == kind;
        return n;
    }
    bool allClosed() const {
        for (const auto& [i, n] : open)
            if (n != 0) return false;
        return true;
    }
};

ui::Modifiers none() { return {}; }

}  // namespace

TEST_CASE("Gesture: one begin and one end per parameter, closed on destruction") {
    MockController c;
    const auto f = params::bandParam(0, BandField::Freq), g = params::bandParam(0, BandField::Gain);
    {
        ui::EditGesture e(c, {f, g});
        for (int i = 0; i < 10; ++i) e.set(f, 8.0 + i * 0.1), e.set(g, i);
    }
    CHECK(c.count('B') == 2);
    CHECK(c.count('E') == 2);
    CHECK(c.count('P') == 20);
    CHECK(c.allClosed());
    ui::EditGesture e2(c, {g});
    e2.set(g, 99.0);
    CHECK(c.t.value(g) == 30.0);  // clamped by the table
}

TEST_CASE("Attachment binds by string id and follows external changes") {
    MockController c;
    ui::ParamAttachment a(c, "band03.gain");
    REQUIRE(a.valid());
    CHECK(a.index() == params::bandParam(2, BandField::Gain));
    double seen = 0;
    ParamSource src{};
    a.onChange = [&](double v, ParamSource s) { seen = v, src = s; };
    c.t.set(a.index(), 4.5, ParamSource::Host);  // host automation
    c.dispatchNotifications();
    CHECK(seen == 4.5);
    CHECK(src == ParamSource::Host);
    CHECK(a.text() == "+4.50 dB");
    CHECK(a.setText("-6"));
    CHECK(c.t.value(a.index()) == -6.0);
    CHECK(c.count('B') == 1);
    CHECK(c.count('E') == 1);
    ui::ParamAttachment bad(c, "band99.gain");
    CHECK_FALSE(bad.valid());
}

TEST_CASE("Geometry round trips and spans 20 Hz to 20 kHz") {
    ui::GraphGeometry g;
    g.setSize(1000.0f, 600.0f);
    g.setRangeDb(12.0);
    CHECK(g.xOfHz(20.0) == doctest::Approx(ui::GraphGeometry::kLeft));
    CHECK(g.xOfHz(20000.0) == doctest::Approx(1000.0f - ui::GraphGeometry::kRight));
    for (double hz : {31.0, 440.0, 9876.0}) CHECK(g.hzOfX(g.xOfHz(hz)) == doctest::Approx(hz).epsilon(1e-5));
    for (double db : {-12.0, 0.0, 7.5}) CHECK(g.dbOfY(g.yOfDb(db)) == doctest::Approx(db).epsilon(1e-5));
}

TEST_CASE("Double-click on empty graph creates a band with defaults in one gesture") {
    MockController c;
    ui::GraphGeometry g;
    g.setSize(1000.0f, 600.0f);
    ui::GraphInteraction m(c, g);
    c.t.set(params::bandParam(0, BandField::Used), 1, ParamSource::Preset);  // band 1 already in use
    c.t.set(params::bandParam(1, BandField::Shape), 5, ParamSource::Preset);  // stale value in band 2
    const float x = g.xOfHz(2000.0), y = g.yOfDb(6.0);
    m.pointerDown(x, y, ui::Button::Left, none(), 2);
    CHECK(m.selected() == 1);
    CHECK(c.t.value(params::bandParam(1, BandField::Used)) == 1.0);
    CHECK(std::exp2(c.t.value(params::bandParam(1, BandField::Freq))) == doctest::Approx(2000.0).epsilon(1e-4));
    CHECK(c.t.value(params::bandParam(1, BandField::Gain)) == doctest::Approx(6.0).epsilon(1e-4));
    CHECK(c.t.value(params::bandParam(1, BandField::Shape)) == 0.0);  // reset to the default (Bell)
    CHECK(c.count('B') == params::kBandFieldCount);  // every field in the same gesture
    CHECK(c.count('E') == params::kBandFieldCount);
    CHECK(c.allClosed());
}

TEST_CASE("A press under the drag threshold is a click; beyond it drags frequency and gain") {
    MockController c;
    ui::GraphGeometry g;
    g.setSize(1000.0f, 600.0f);
    ui::GraphInteraction m(c, g);
    m.createBandAt(g.xOfHz(1000.0), g.yOfDb(0.0));
    c.log.clear();
    const auto n = m.node(0);
    m.pointerDown(n.x + 2.0f, n.y, ui::Button::Left, none(), 1);
    m.pointerDrag(n.x + 3.5f, n.y + 1.0f, none());  // 1.8 px: still a click
    CHECK(c.log.empty());
    m.pointerDrag(n.x + 2.0f + 50.0f, n.y - 40.0f, none());  // a drag
    m.pointerDrag(n.x + 2.0f + 100.0f, n.y - 80.0f, none());
    m.pointerUp(0, 0, none());
    CHECK(c.count('B') == 2);  // frequency and gain, one gesture each
    CHECK(c.count('E') == 2);
    CHECK(c.allClosed());
    // The grab offset is kept: the node lands exactly under the pointer offset.
    CHECK(m.node(0).x == doctest::Approx(n.x + 100.0f).epsilon(1e-4));
    CHECK(m.node(0).y == doctest::Approx(n.y - 80.0f).epsilon(1e-4));
}

TEST_CASE("Gainless shapes drag Q vertically; the wheel changes Q; Alt-click toggles Enabled") {
    MockController c;
    ui::GraphGeometry g;
    g.setSize(1000.0f, 600.0f);
    ui::GraphInteraction m(c, g);
    m.createBandAt(g.xOfHz(500.0), g.yOfDb(0.0));
    c.t.set(params::bandParam(0, BandField::Shape), static_cast<int>(dsp::Shape::LowCut), ParamSource::Preset);
    const auto q0 = c.t.value(params::bandParam(0, BandField::Q));
    const auto gain0 = c.t.value(params::bandParam(0, BandField::Gain));
    auto n = m.node(0);
    m.pointerDown(n.x, n.y, ui::Button::Left, none(), 1);
    m.pointerDrag(n.x, n.y - 50.0f, none());
    m.pointerUp(0, 0, none());
    CHECK(c.t.value(params::bandParam(0, BandField::Q)) == doctest::Approx(q0 + 50 * ui::GraphInteraction::kDragQPerPx));
    CHECK(c.t.value(params::bandParam(0, BandField::Gain)) == gain0);
    const auto q1 = c.t.value(params::bandParam(0, BandField::Q));
    n = m.node(0);
    m.wheel(n.x, n.y, 2.0, none());
    CHECK(c.t.value(params::bandParam(0, BandField::Q)) == doctest::Approx(q1 + 2 * ui::GraphInteraction::kWheelQStep));
    m.pointerDown(n.x, n.y, ui::Button::Left, {false, true, false}, 1);  // Alt-click
    m.pointerUp(0, 0, none());
    CHECK(c.t.value(params::bandParam(0, BandField::Enabled)) == 0.0);
    CHECK(c.allClosed());
}

TEST_CASE("All 24 bands in use: double-click creates nothing") {
    MockController c;
    ui::GraphGeometry g;
    ui::GraphInteraction m(c, g);
    for (int b = 0; b < params::kBandCount; ++b) c.t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    CHECK(m.createBandAt(100.0f, 100.0f) == -1);
}

TEST_CASE("Response curve: matches the analog prototype and recomputes only dirty bands") {
    params::ParameterTable t;
    ui::ResponseCurve curve;
    curve.setFrequencies({100.0, 1000.0, 10000.0});
    curve.update(t);
    CHECK(curve.combinedDb()[1] == 0.0);  // no bands
    t.set(params::bandParam(0, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(0, BandField::Gain), 6.0, ParamSource::Preset);
    curve.markDirty(0);
    CHECK(curve.update(t));
    CHECK(curve.combinedDb()[1] == doctest::Approx(6.0).epsilon(1e-6));  // Bell at 1 kHz, +6 dB
    CHECK_FALSE(curve.update(t));                                         // nothing dirty: no work
    t.set(params::globalParam(params::GlobalParam::GainScale), 50.0, ParamSource::Preset);
    CHECK(curve.update(t));  // Gain Scale change recomputes every band
    CHECK(curve.combinedDb()[1] == doctest::Approx(3.0).epsilon(1e-6));
    t.set(params::bandParam(0, BandField::Enabled), 0, ParamSource::Preset);
    curve.markDirty(0);
    curve.update(t);
    CHECK(curve.combinedDb()[1] == 0.0);
}

namespace {
// Serves snapshot frames like the plugin does (one reader, newest frame or nothing new).
class FeedController : public MockController {
public:
    analysis::LevelSnapshot next{};
    bool fresh = false;
    const analysis::LevelSnapshot* pullLevels() noexcept override {
        if (!fresh) return nullptr;
        fresh = false;
        return &next;
    }
    void serve(uint64_t stamp, float outPeakDb) {
        next.header.stamp = stamp;
        next.channels = 2;
        next.output[0].peakDb = outPeakDb;
        fresh = true;
    }
};
}  // namespace

TEST_CASE("Snapshot poller: floor first, stamps checked, redraw only on visible change") {
    FeedController c;
    ui::SnapshotPoller p;
    CHECK(p.levels().output[0].peakDb == analysis::kFloorDb);  // area 12 R11
    CHECK(p.poll(c) == ui::SnapshotPoller::kNone);             // nothing published yet
    c.serve(1, -20.0f);
    CHECK(p.poll(c) == ui::SnapshotPoller::kLevels);
    CHECK(p.levels().output[0].peakDb == -20.0f);
    c.serve(2, -20.04f);  // below the redraw step
    CHECK(p.poll(c) == ui::SnapshotPoller::kNone);
    CHECK(p.framesTaken() == 2);
    c.serve(2, -3.0f);  // same stamp again: never drawn as new
    CHECK(p.poll(c) == ui::SnapshotPoller::kNone);
    CHECK(p.staleSkipped() == 1);
    CHECK(p.levels().output[0].peakDb == doctest::Approx(-20.04f));
    c.serve(3, -3.0f);
    CHECK(p.poll(c) == ui::SnapshotPoller::kLevels);
    CHECK(p.polls() == 5);
}
