#include "amq/ui/band_panel.hpp"

#include "amq/dsp/dynamics.hpp"
#include "amq/params/shape_info.hpp"

#include <cmath>
#include <cstdio>

namespace amq::ui {

using params::BandField;

namespace {

std::string msText(double seconds) {
    char buf[48];
    if (seconds < 0.01) std::snprintf(buf, sizeof(buf), "%.2f ms", seconds * 1e3);
    else if (seconds < 1.0) std::snprintf(buf, sizeof(buf), "%.1f ms", seconds * 1e3);
    else std::snprintf(buf, sizeof(buf), "%.2f s", seconds);
    return buf;
}

}  // namespace

std::vector<PanelControl> bandPanel(const params::ParameterTable& t, int b) {
    std::vector<PanelControl> c;
    if (b < 0 || b >= params::kBandCount) return c;
    auto id = [&](BandField f) { return params::bandParam(b, f); };
    auto val = [&](BandField f) { return t.value(id(f)); };
    auto add = [&](const char* section, const char* label, ControlKind k, BandField f, bool visible = true,
                   bool enabled = true) {
        PanelControl p;
        p.section = section, p.label = label, p.kind = k, p.id = id(f);
        p.visible = visible, p.enabled = enabled;
        p.text = params::displayText(t, p.id);
        c.push_back(p);
        return c.size() - 1;
    };
    const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(val(BandField::Shape))));
    const bool gainShape = info.hasGain;
    const bool dynamic = val(BandField::DynEnabled) >= 0.5;
    const bool autoOn = val(BandField::DynAuto) >= 0.5;
    const bool spectral = val(BandField::Spectral) >= 0.5;
    const bool free = val(BandField::ScMode) >= 0.5;
    const double hz = std::exp2(val(BandField::Freq));

    add("Band", "Shape", ControlKind::Choice, BandField::Shape);
    add("Band", "Frequency", ControlKind::Knob, BandField::Freq);
    add("Band", "Gain", ControlKind::Knob, BandField::Gain, info.hasGain);
    add("Band", "Q", ControlKind::Knob, BandField::Q, info.hasQ);
    add("Band", "Slope", ControlKind::Choice, BandField::Slope, info.hasSlope);
    add("Band", "Placement", ControlKind::Choice, BandField::Placement);
    add("Band", "Enabled", ControlKind::Toggle, BandField::Enabled);
    add("Band", "Solo", ControlKind::Toggle, BandField::Solo);

    add("Dynamics", "Dynamic", ControlKind::Toggle, BandField::DynEnabled, gainShape);
    add("Dynamics", "Range", ControlKind::Knob, BandField::DynRange, gainShape, dynamic);
    add("Dynamics", "Auto", ControlKind::Toggle, BandField::DynAuto, gainShape, dynamic);
    add("Dynamics", "Threshold", ControlKind::Knob, BandField::Threshold, gainShape, dynamic && !autoOn);
    const size_t a = add("Dynamics", "Attack", ControlKind::Knob, BandField::Attack, gainShape, dynamic && !autoOn);
    const size_t r = add("Dynamics", "Release", ControlKind::Knob, BandField::Release, gainShape, dynamic && !autoOn);
    // Effective times (area 06 Phase 3 tooltip): what the knob means at this band's frequency; in
    // Auto, the timing in use is the knob-50 default.
    const double ka = autoOn ? dsp::kDefaultTimingKnob : val(BandField::Attack);
    const double kr = autoOn ? dsp::kDefaultTimingKnob : val(BandField::Release);
    c[a].tooltip = "Attack " + msText(dsp::attackSeconds(ka, hz)) + " at this band's frequency";
    c[r].tooltip = "Release " + msText(dsp::releaseSeconds(kr)) + " (90 % time)";

    add("Side Chain", "External", ControlKind::Toggle, BandField::ScExternal, gainShape, dynamic);
    add("Side Chain", "Mode", ControlKind::Choice, BandField::ScMode, gainShape, dynamic);
    add("Side Chain", "Low", ControlKind::Knob, BandField::ScLow, gainShape && free, dynamic);
    add("Side Chain", "High", ControlKind::Knob, BandField::ScHigh, gainShape && free, dynamic);
    add("Side Chain", "Listen", ControlKind::Toggle, BandField::ScListen, gainShape);

    add("Spectral", "Spectral", ControlKind::Toggle, BandField::Spectral, gainShape, dynamic);
    add("Spectral", "Density", ControlKind::Knob, BandField::SpectralDensity, gainShape, dynamic && spectral);
    add("Spectral", "Tilt", ControlKind::Toggle, BandField::SpectralTilt, gainShape, dynamic && spectral);

    PanelControl del;
    del.section = "Band", del.label = "Delete", del.kind = ControlKind::Button, del.button = PanelButton::Delete;
    c.push_back(del);
    PanelControl clr;
    clr.section = "Dynamics", clr.label = "Clear Dynamics", clr.kind = ControlKind::Button,
    clr.button = PanelButton::ClearDynamics;
    clr.visible = gainShape, clr.enabled = dynamic || val(BandField::DynRange) != 0.0;
    c.push_back(clr);
    return c;
}

std::vector<ChoiceItem> choiceItems(const params::ParameterTable& t, params::ParamIndex id) {
    std::vector<ChoiceItem> items;
    const auto* s = params::specAt(id);
    if (!s || s->type != params::ParamType::Choice) return items;
    const int current = static_cast<int>(t.value(id));
    // Shape and Slope depend on each other: find the band's shape for the slope rule.
    const auto band = static_cast<int>((id - params::bandParam(0, BandField::Used)) / params::kBandFieldCount);
    const bool isSlope = band >= 0 && band < params::kBandCount && id == params::bandParam(band, BandField::Slope);
    const bool isShape = band >= 0 && band < params::kBandCount && id == params::bandParam(band, BandField::Shape);
    const auto shape = isSlope ? static_cast<dsp::Shape>(static_cast<int>(t.value(params::bandParam(band, BandField::Shape))))
                               : dsp::Shape::Bell;
    for (size_t i = 0; i < s->choices.size(); ++i) {
        ChoiceItem it;
        it.value = static_cast<int>(i);
        it.label = isShape ? std::string(params::shapeInfo(static_cast<dsp::Shape>(i)).name) : s->choices[i];
        it.checked = it.value == current;
        if (isSlope) it.enabled = dsp::slopeAllowed(shape, static_cast<dsp::Slope>(i));
        items.push_back(it);
    }
    return items;
}

}  // namespace amq::ui
