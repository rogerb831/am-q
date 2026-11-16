// Parameter table tests (areas 09, 10, 11, 15, 21 acceptance tests).
#include "amq/params/mappings.hpp"
#include "amq/params/parameter_table.hpp"
#include "amq/params/shape_info.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

using namespace amq::params;

TEST_CASE("Table shape: 24 complete band blocks plus globals, no reserved ids") {
    const auto& specs = parameterSpecs();
    CHECK(specs.size() == kParamCount);
    CHECK(kParamCount == 24 * 23 + 21);
    for (int b = 0; b < kBandCount; ++b) {
        char id[32];
        std::snprintf(id, sizeof(id), "band%02d.spectralTilt", b + 1);
        REQUIRE(findById(id).has_value());
        CHECK(*findById(id) == bandParam(b, BandField::SpectralTilt));
    }
    CHECK(specs[globalParam(GlobalParam::Bypass)].isBypass);
    for (const auto& r : reservedIds()) CHECK_FALSE(findById(r).has_value());
}

TEST_CASE("Host ids match the frozen golden list") {
    std::ifstream f(AMQ_HOST_IDS_GOLDEN);
    REQUIRE(f.good());
    std::string line;
    std::set<std::string> seen;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string id;
        uint32_t host = 0;
        ls >> id >> host;
        const auto idx = findById(id);
        INFO(id);
        REQUIRE(idx.has_value());
        CHECK(specAt(*idx)->hostId == host);
        CHECK(stableHostId(id) == host);
        CHECK(findByHostId(host) == idx);
        seen.insert(id);
    }
    CHECK(seen.size() == kParamCount);  // every exposed parameter is frozen in the list
}

TEST_CASE("Bounds-checked lookups return errors") {
    ParameterTable t;
    CHECK(specAt(kParamCount) == nullptr);
    CHECK_FALSE(findById("band25.freq").has_value());
    CHECK_FALSE(findByHostId(0xFFFFFFFFu).has_value());
    CHECK(t.set(kParamCount, 1.0, ParamSource::Gui) == SetResult::BadIndex);
    CHECK_FALSE(t.get(kParamCount).has_value());
}

TEST_CASE("Clamp and epsilon from every source") {
    ParameterTable t;
    const auto gain = bandParam(2, BandField::Gain);
    for (auto src : {ParamSource::Gui, ParamSource::Host, ParamSource::Preset, ParamSource::EqMatch, ParamSource::Undo}) {
        t.set(gain, 99.0, src);
        CHECK(t.value(gain) == 30.0);
        t.set(gain, -99.0, src);
        CHECK(t.value(gain) == -30.0);
    }
    t.dispatchNotifications();
    CHECK(t.set(gain, -30.0 + 1e-12, ParamSource::Gui) == SetResult::Unchanged);
    CHECK_FALSE(t.hasPendingNotifications());
    const auto shape = bandParam(0, BandField::Shape);
    t.set(shape, 3.4, ParamSource::Host);
    CHECK(t.value(shape) == 3.0);
    t.set(shape, 42.0, ParamSource::Host);
    CHECK(t.value(shape) == 9.0);
    t.set(gain, std::nan(""), ParamSource::Host);
    CHECK(t.value(gain) == 0.0);  // NaN falls back to the default
}

TEST_CASE("Normalised round trip: continuous within 1e-6 of range, indexed exact") {
    for (const auto& s : parameterSpecs()) {
        INFO(s.id);
        if (s.type == ParamType::Continuous) {
            for (int i = 0; i <= 1000; ++i) {
                const double plain = s.min + (s.max - s.min) * i / 1000.0;
                const double back = fromNormalised(s, toNormalised(s, plain));
                // Output Level's fade region is a curve; compare in its host domain instead.
                if (s.unit == Unit::OutputLevelDb)
                    CHECK(std::abs(toHost(s, back) - toHost(s, plain)) < 1e-6);
                else
                    CHECK(std::abs(back - plain) < 1e-6 * (s.max - s.min));
            }
        } else {
            for (int i = 0; i <= static_cast<int>(s.max); ++i) CHECK(fromNormalised(s, toNormalised(s, i)) == i);
            if (s.max > 0) CHECK(toNormalised(s, s.max) == 1.0);
        }
    }
}

TEST_CASE("Q mapping: exact endpoints and round trip") {
    CHECK(qFromPosition(0.0) == doctest::Approx(0.025).epsilon(1e-12));
    CHECK(qFromPosition(0.5) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(qFromPosition(1.0) == doctest::Approx(40.0).epsilon(1e-12));
    for (int i = 0; i <= 10000; ++i) {
        const double n = i / 10000.0;
        const double q = qFromPosition(n);
        CHECK(std::abs(qFromPosition(positionFromQ(q)) - q) / q < 1e-6);
    }
}

TEST_CASE("Output Level: linear in dB above -36, monotonic fade to silence below") {
    CHECK(outputLevelDbFromPosition(1.0) == doctest::Approx(36.0));
    CHECK(outputLevelDbFromPosition(kOutputFadeStart) == doctest::Approx(-36.0));
    CHECK(outputLevelDbFromPosition(0.0) == kOutputSilenceDb);
    CHECK(outputLevelGain(kOutputSilenceDb) == 0.0);
    CHECK(outputLevelPositionFromDb(0.0) == doctest::Approx(kOutputFadeStart + (1 - kOutputFadeStart) * 0.5));
    double prev = -1e9;
    for (int i = 0; i <= 10000; ++i) {
        const double p = i / 10000.0;
        const double db = outputLevelDbFromPosition(p);
        CHECK(db >= prev);
        prev = db;
        if (p > 0) CHECK(outputLevelPositionFromDb(db) == doctest::Approx(p).epsilon(1e-9));
    }
    // Linear in dB above -36: equal position steps give equal dB steps.
    const double d1 = outputLevelDbFromPosition(0.5) - outputLevelDbFromPosition(0.4);
    const double d2 = outputLevelDbFromPosition(0.9) - outputLevelDbFromPosition(0.8);
    CHECK(d1 == doctest::Approx(d2));
}

TEST_CASE("Formatter and parser round trip for every parameter") {
    for (const auto& s : parameterSpecs()) {
        INFO(s.id);
        const int steps = s.type == ParamType::Continuous ? 50 : static_cast<int>(s.max);
        for (int i = 0; i <= steps; ++i) {
            const double plain = s.type == ParamType::Continuous ? fromNormalised(s, i / double(steps)) : i;
            const std::string text = formatPlain(s, plain);
            INFO(text);
            const auto parsed = parsePlain(s, text);
            REQUIRE(parsed.has_value());
            // Re-formatting the parsed value gives the same text: equal within display precision.
            CHECK(formatPlain(s, *parsed) == text);
        }
    }
    const auto& freq = parameterSpecs()[bandParam(0, BandField::Freq)];
    CHECK(*parsePlain(freq, "1k") == doctest::Approx(std::log2(1000.0)));
    CHECK(*parsePlain(freq, "440 Hz") == doctest::Approx(std::log2(440.0)));
    CHECK_FALSE(parsePlain(freq, "loud").has_value());
    const auto& shape = parameterSpecs()[bandParam(0, BandField::Shape)];
    CHECK(formatPlain(shape, 0) == "Bell");  // readable choice names (area 21 R15)
}

TEST_CASE("Output Pan text follows Pan Mode (area 04 R6)") {
    const auto& pan = parameterSpecs()[globalParam(GlobalParam::OutputPan)];
    const DisplayContext lr{}, ms{true};
    CHECK(formatPlain(pan, -25.0, lr) == "25.0 L");
    CHECK(formatPlain(pan, 50.0, lr) == "50.0 R");
    CHECK(formatPlain(pan, -25.0, ms) == "25.0 M");
    CHECK(formatPlain(pan, 50.0, ms) == "50.0 S");
    CHECK(formatPlain(pan, 0.0, ms) == "C");
    CHECK(formatPlain(pan, 50.0) == "50.0 R");  // no context: L/R
    // Parsing takes either form, whatever the mode.
    CHECK(*parsePlain(pan, "30 M") == -30.0);
    CHECK(*parsePlain(pan, "30 s") == 30.0);
    CHECK(*parsePlain(pan, "30 L") == -30.0);
    // Round trip in M/S text.
    for (int i = 0; i <= 40; ++i) {
        const double v = -100.0 + 5.0 * i;
        const std::string text = formatPlain(pan, v, ms);
        INFO(text);
        CHECK(formatPlain(pan, *parsePlain(pan, text), ms) == text);
    }
    // The table gives the context.
    ParameterTable t;
    const auto ip = globalParam(GlobalParam::OutputPan);
    t.set(ip, 40.0, ParamSource::Preset);
    CHECK(displayText(t, ip) == "40.0 R");
    t.set(globalParam(GlobalParam::PanMode), 1, ParamSource::Preset);
    CHECK(displayContext(t).panMidSide);
    CHECK(displayText(t, ip) == "40.0 S");
    CHECK(displayText(t, globalParam(GlobalParam::PanMode)) == "M/S");
}

TEST_CASE("No-op writes and coalescing") {
    struct Counter : ParameterListener {
        int calls = 0;
        double last = 0;
        ParamSource src{};
        void parameterChanged(ParamIndex, double v, ParamSource s) override { ++calls, last = v, src = s; }
    } c;
    ParameterTable t;
    t.addListener(&c);
    const auto q = bandParam(5, BandField::Q);
    for (int i = 0; i < 1000; ++i) t.set(q, 0.7, ParamSource::Gui);
    CHECK(t.dispatchNotifications() == 1);
    CHECK(c.calls == 1);
    CHECK(t.dispatchNotifications() == 0);
    for (int i = 1; i <= 10000; ++i) t.set(q, i / 10000.0, ParamSource::Host);
    CHECK(t.dispatchNotifications() == 1);
    CHECK(c.calls == 2);
    CHECK(c.last == 1.0);
    CHECK(c.src == ParamSource::Host);
    t.removeListener(&c);
}

TEST_CASE("Writes from another thread are delivered on the dispatching thread") {
    struct Rec : ParameterListener {
        std::thread::id thread;
        int calls = 0;
        void parameterChanged(ParamIndex, double, ParamSource) override { thread = std::this_thread::get_id(), ++calls; }
    } r;
    ParameterTable t;
    t.addListener(&r);
    std::thread w([&] {
        for (int b = 0; b < kBandCount; ++b) t.set(bandParam(b, BandField::Gain), 6.0, ParamSource::Host);
    });
    w.join();
    CHECK(t.dispatchNotifications() == kBandCount);
    CHECK(r.thread == std::this_thread::get_id());
}

TEST_CASE("Shape descriptors agree with the DSP") {
    for (int i = 0; i < amq::dsp::kShapeCount; ++i) {
        const auto s = static_cast<amq::dsp::Shape>(i);
        const auto info = shapeInfo(s);
        CHECK(info.hasGain == amq::dsp::shapeHasGain(s));
        CHECK(info.hasQ == amq::dsp::shapeUsesQ(s));
        CHECK(info.hasSlope == amq::dsp::shapeUsesSlope(s));
        CHECK(parameterSpecs()[bandParam(0, BandField::Shape)].choices[static_cast<size_t>(i)] == info.name);
    }
    CHECK(shapeInfo(amq::dsp::Shape::Bell).family == ShapeFamily::BellType);
    CHECK(parameterSpecs()[bandParam(0, BandField::Shape)].def == 0.0);  // Bell is the default (area 15 R10)
}

TEST_CASE("Shape ids and names are pinned (area 15 R7)") {
    // Stored ids never renumber; a new shape takes the next free id. Never edit these lines.
    using amq::dsp::Shape;
    static_assert(static_cast<int>(Shape::Bell) == 0 && static_cast<int>(Shape::LowShelf) == 1 &&
                  static_cast<int>(Shape::LowCut) == 2 && static_cast<int>(Shape::HighShelf) == 3 &&
                  static_cast<int>(Shape::HighCut) == 4 && static_cast<int>(Shape::Notch) == 5 &&
                  static_cast<int>(Shape::BandPass) == 6 && static_cast<int>(Shape::TiltShelf) == 7 &&
                  static_cast<int>(Shape::FlatTilt) == 8 && static_cast<int>(Shape::AllPass) == 9);
    static_assert(amq::dsp::kShapeCount >= 10);
    const char* const names[] = {"Bell",      "Low Shelf", "Low Cut",    "High Shelf", "High Cut",
                                 "Notch",     "Band Pass", "Tilt Shelf", "Flat Tilt",  "All Pass"};
    const auto& choices = parameterSpecs()[bandParam(0, BandField::Shape)].choices;
    REQUIRE(choices.size() >= 10);
    for (size_t i = 0; i < 10; ++i) CHECK(choices[i] == names[i]);
}

TEST_CASE("Clamping, reset and same-value rules (area 08 acceptance 'Clamping'; area 21 R16)") {
    ParameterTable t;
    for (ParamIndex i = 0; i < kParamCount; ++i) {
        const auto& s = *specAt(i);
        for (auto src : {ParamSource::Host, ParamSource::Gui, ParamSource::Preset}) {
            t.set(i, s.max + 1000.0, src);
            CHECK(t.value(i) == clampPlain(s, s.max + 1000.0));
            CHECK(t.value(i) == s.max);
            t.set(i, s.min - 1000.0, src);
            CHECK(t.value(i) == s.min);
        }
    }
    const auto& shape = *specAt(bandParam(0, BandField::Shape));
    CHECK(clampPlain(shape, 2.6) == 3.0);  // choices round to a whole index
    CHECK(sameValue(shape, 2.0, 2.0));
    CHECK_FALSE(sameValue(shape, 2.0, 3.0));
    const auto& gain = *specAt(bandParam(0, BandField::Gain));
    CHECK(sameValue(gain, 1.0, 1.0 + 1e-12));
    CHECK_FALSE(sameValue(gain, 1.0, 1.001));
    t.resetToDefaults(ParamSource::Preset);
    bool defaults = true;
    for (ParamIndex i = 0; i < kParamCount; ++i) defaults &= t.value(i) == specAt(i)->def;
    CHECK(defaults);
}
