#include "amq/ui/band_menu.hpp"

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
    m.push_back({menu::SelectAll, "Select All", true});
    m.push_back({menu::ClearSelection, "Clear Selection", has});
    const bool enabled = all(ops, targets, BandField::Enabled);
    m.push_back({enabled ? menu::Disable : menu::Enable, enabled ? "Disable" : "Enable", has, false, true});
    const bool gain = anyGainShape(ops, targets);
    m.push_back({menu::InvertGain, "Invert Gain", has && gain});
    const bool dynamic = all(ops, targets, BandField::DynEnabled);
    m.push_back({dynamic ? menu::ClearDynamics : menu::MakeDynamic, dynamic ? "Clear Dynamics" : "Make Dynamic",
                 has && gain, false, true});
    const bool spectral = all(ops, targets, BandField::Spectral);
    m.push_back({spectral ? menu::ClearSpectral : menu::MakeSpectral, spectral ? "Clear Spectral" : "Make Spectral",
                 has && gain});
    const int shape = common(ops, targets, BandField::Shape);
    for (int s = 0; s < dsp::kShapeCount; ++s)
        m.push_back({menu::ShapeBase + s, std::string(dsp::shapeName(static_cast<dsp::Shape>(s))), has, shape == s,
                     s == 0, "Shape"});
    const int slope = common(ops, targets, BandField::Slope);
    for (int s = 0; s < dsp::kSlopeCount; ++s) {
        bool ok = has;  // a slope is offered only if every target's shape allows it
        for (int b : targets)
            ok = ok && dsp::slopeAllowed(static_cast<dsp::Shape>(static_cast<int>(val(ops, b, BandField::Shape))),
                                        static_cast<dsp::Slope>(s));
        m.push_back({menu::SlopeBase + s, std::string(dsp::slopeName(static_cast<dsp::Slope>(s))), ok, slope == s, false,
                     "Slope"});
    }
    static const char* kPlacement[] = {"Stereo", "Left", "Right", "Mid", "Side"};
    const int placement = common(ops, targets, BandField::Placement);
    for (int p = 0; p < 5; ++p)
        m.push_back({menu::PlacementBase + p, kPlacement[p], has, placement == p, false, "Placement"});
    m.push_back({menu::Cut, "Cut", has, false, true});
    m.push_back({menu::Copy, "Copy", has});
    m.push_back({menu::Paste, plural || has ? "Paste Over" : "Paste", clipboardHasBands});
    m.push_back({menu::Split, "Split", false, false, true});  // behaviour not defined yet
    m.push_back({menu::Delete, plural ? "Delete " + std::to_string(targets.size()) + " Bands" : "Delete Band", has,
                 false, true});
    m.push_back({menu::DeleteDisabled, "Delete Disabled Bands", !ops.disabledBands().empty()});
    m.push_back({menu::DeleteAll, "Delete All Bands", !ops.usedBands().empty()});
    return m;
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
