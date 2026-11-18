#include "amq/ui/band_menu.hpp"

#include "amq/ui/band_panel.hpp"

#include "amq/params/shape_info.hpp"

#include <cctype>
#include <string>

namespace amq::ui {

using params::BandField;

namespace {

double val(const BandOps& ops, int b, BandField f) {
    return ops.table().value(params::bandParam(b, f));
}

bool all(const BandOps& ops, const BandOps::Bands& t, BandField f) {
    for (int b : t)
        if (val(ops, b, f) < 0.5) return false;
    return !t.empty();
}

bool anyGainShape(const BandOps& ops, const BandOps::Bands& t) {
    for (int b : t)
        if (dsp::shapeHasGain(static_cast<dsp::Shape>(static_cast<int>(val(ops, b, BandField::Shape))))) return true;
    return false;
}

int common(const BandOps& ops, const BandOps::Bands& t, BandField f) {  // -1 when the targets differ
    if (t.empty()) return -1;
    const int v = static_cast<int>(val(ops, t[0], f));
    for (int b : t)
        if (static_cast<int>(val(ops, b, f)) != v) return -1;
    return v;
}

}  // namespace

std::vector<MenuItem> bandMenu(const BandOps& ops, const BandOps::Bands& targets, bool clipboardHasBands) {
    std::vector<MenuItem> m;
    const bool has = !targets.empty();
    const bool plural = targets.size() > 1;
    auto add = [&](int action, std::string label, bool enabled) -> MenuItem& {
        MenuItem it;
        it.action = action, it.label = std::move(label), it.enabled = enabled;
        m.push_back(std::move(it));
        return m.back();
    };
    auto any = [&](BandField f) {
        for (int b : targets)
            if (val(ops, b, f) >= 0.5) return true;
        return false;
    };
    // Group 1: checkboxes and Invert Gain.
    const bool enabled = all(ops, targets, BandField::Enabled);
    auto& en = add(enabled ? menu::Disable : menu::Enable, "Enabled", has);
    en.checkbox = true, en.checked = enabled, en.mixed = !enabled && any(BandField::Enabled), en.hint = "Alt-click";
    const bool gain = anyGainShape(ops, targets);
    add(menu::InvertGain, "Invert Gain", has && gain);
    const bool dynamic = all(ops, targets, BandField::DynEnabled);
    auto& dy = add(dynamic ? menu::ClearDynamics : menu::MakeDynamic, "Make Dynamic", has && gain);
    dy.checkbox = true, dy.checked = dynamic, dy.mixed = !dynamic && any(BandField::DynEnabled);
    const bool spectral = all(ops, targets, BandField::Spectral);
    auto& sp = add(spectral ? menu::ClearSpectral : menu::MakeSpectral, "Make Spectral", has && gain);
    sp.checkbox = true, sp.checked = spectral, sp.mixed = !spectral && any(BandField::Spectral);
    // Group 2: Shape, Slope, Placement submenus. Labels and order from the table (area 11).
    const int shape = common(ops, targets, BandField::Shape);
    const auto shapes = choiceItems(ops.table(), params::bandParam(0, BandField::Shape));
    for (const auto& it : shapes) {
        auto& x = add(menu::ShapeBase + it.value, it.label, has);
        x.checked = shape == it.value, x.submenu = "Shape", x.separatorBefore = it.value == 0;
    }
    const int slope = common(ops, targets, BandField::Slope);
    const auto& slopes = params::specAt(params::bandParam(0, BandField::Slope))->choices;
    for (int s = 0; s < static_cast<int>(slopes.size()); ++s) {
        bool ok = has;  // a slope is offered only if every target's shape allows it
        for (int b : targets)
            ok = ok && dsp::slopeAllowed(static_cast<dsp::Shape>(static_cast<int>(val(ops, b, BandField::Shape))),
                                        static_cast<dsp::Slope>(s));
        auto& x = add(menu::SlopeBase + s, slopes[static_cast<size_t>(s)], ok);
        x.checked = slope == s, x.submenu = "Slope";
    }
    const auto& places = params::specAt(params::bandParam(0, BandField::Placement))->choices;
    const int placement = common(ops, targets, BandField::Placement);
    for (int p = 0; p < static_cast<int>(places.size()); ++p) {
        auto& x = add(menu::PlacementBase + p, places[static_cast<size_t>(p)], has);
        x.checked = placement == p, x.submenu = "Placement";
    }
    // Group 3: clipboard and selection.
    auto& cut = add(menu::Cut, "Cut", has);
    cut.separatorBefore = true, cut.hint = "Ctrl X";
    add(menu::Copy, "Copy", has).hint = "Ctrl C";
    add(menu::Paste, plural || has ? "Paste Over" : "Paste", clipboardHasBands).hint = "Ctrl V";
    add(menu::SelectAll, "Select All", true).hint = "Ctrl A";
    // Group 4.
    add(menu::Split, "Split", false).separatorBefore = true;  // behaviour not defined yet (area 14)
    add(menu::Delete, plural ? "Delete " + std::to_string(targets.size()) + " Bands" : "Delete Band", has).hint = "Del";
    return m;
}

std::string bandMenuCaption(const BandOps& ops, const BandOps::Bands& t) {
    auto upper = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    if (t.size() == 1) {
        const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(val(ops, t[0], BandField::Shape))));
        return "BAND #" + std::to_string(t[0] + 1) + " \xC2\xB7 " + upper(std::string(info.name));
    }
    std::string s = std::to_string(t.size()) + " BANDS \xC2\xB7 ";
    for (size_t i = 0; i < t.size() && i < 4; ++i) s += (i ? ", #" : "#") + std::to_string(t[i] + 1);
    if (t.size() > 4) s += ", \xE2\x80\xA6";
    return s;
}

std::vector<MenuItem> graphMenu(const BandOps& ops, bool clipboardHasBands) {
    std::vector<MenuItem> m;
    m.push_back({menu::SelectAll, "Select All", !ops.usedBands().empty()});
    m.push_back({menu::Paste, "Paste", clipboardHasBands, false, true});
    m.push_back({menu::DeleteDisabled, "Delete Disabled Bands", !ops.disabledBands().empty(), false, true});
    m.push_back({menu::DeleteAll, "Delete All Bands", !ops.usedBands().empty()});
    return m;
}

bool activate(BandOps& ops, int a, const BandOps::Bands& t, std::string& clipboard, std::string* message) {
    if (a >= menu::PlacementBase) {
        ops.setPlacement(t, a - menu::PlacementBase);
        return true;
    }
    if (a >= menu::SlopeBase) {
        ops.setSlope(t, static_cast<dsp::Slope>(a - menu::SlopeBase));
        return true;
    }
    if (a >= menu::ShapeBase) {
        ops.setShape(t, static_cast<dsp::Shape>(a - menu::ShapeBase));
        return true;
    }
    switch (a) {
        case menu::Enable: ops.setEnabled(t, true); return true;
        case menu::Disable: ops.setEnabled(t, false); return true;
        case menu::InvertGain: ops.invertGain(t); return true;
        case menu::MakeDynamic: ops.makeDynamic(t); return true;
        case menu::ClearDynamics: ops.clearDynamics(t); return true;
        case menu::MakeSpectral: ops.makeSpectral(t); return true;
        case menu::ClearSpectral: ops.clearSpectral(t); return true;
        case menu::Cut: clipboard = ops.cut(t); return true;
        case menu::Copy: clipboard = ops.copy(t); return true;
        case menu::Paste: return ops.paste(clipboard, t, message);
        case menu::Delete: ops.remove(t); return true;
        case menu::DeleteDisabled: ops.remove(ops.disabledBands()); return true;
        case menu::DeleteAll: ops.remove(ops.usedBands()); return true;
        default: return false;  // selection actions and Split
    }
}

}  // namespace amq::ui
