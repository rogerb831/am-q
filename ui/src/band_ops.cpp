#include "amq/ui/band_ops.hpp"

#include "amq/ui/edit_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace amq::ui {

using params::BandField;

namespace {

std::string fieldName(int b, int f) {
    const auto& id = params::specAt(params::bandParam(b, static_cast<BandField>(f)))->id;
    return id.substr(id.find('.') + 1);
}

double def(int b, BandField f) { return params::specAt(params::bandParam(b, f))->def; }

}  // namespace

BandOps::Bands BandOps::targets(const Bands& selection, int focused) {
    if (!selection.empty()) return selection;
    if (focused >= 0 && focused < params::kBandCount) return {focused};
    return {};
}

double BandOps::value(int b, BandField f) const { return c_.table().value(params::bandParam(b, f)); }
bool BandOps::used(int b) const { return value(b, BandField::Used) >= 0.5; }

BandOps::Bands BandOps::usedBands() const {
    Bands r;
    for (int b = 0; b < params::kBandCount; ++b)
        if (used(b)) r.push_back(b);
    return r;
}

BandOps::Bands BandOps::disabledBands() const {
    Bands r;
    for (int b = 0; b < params::kBandCount; ++b)
        if (used(b) && value(b, BandField::Enabled) < 0.5) r.push_back(b);
    return r;
}

int BandOps::freeBands() const { return params::kBandCount - static_cast<int>(usedBands().size()); }

void BandOps::write(const std::vector<std::pair<params::ParamIndex, double>>& values) {
    if (values.empty()) return;
    std::vector<params::ParamIndex> ids;
    for (const auto& [i, v] : values) ids.push_back(i);
    EditGesture g;
    g.open(c_, ids);
    for (const auto& [i, v] : values) g.set(i, v);
}

int BandOps::create(double hz, double gainDb, dsp::Shape shape) {
    int b = 0;
    while (b < params::kBandCount && used(b)) ++b;
    if (b == params::kBandCount) return -1;
    UndoGroup group(c_, "Create Band");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int f = 0; f < params::kBandFieldCount; ++f) {
        const auto bf = static_cast<BandField>(f);
        v.emplace_back(params::bandParam(b, bf), def(b, bf));  // every default, in this one step
    }
    for (auto& [i, x] : v) {
        if (i == params::bandParam(b, BandField::Used)) x = 1.0;
        if (i == params::bandParam(b, BandField::Freq)) x = std::log2(hz);
        if (i == params::bandParam(b, BandField::Gain)) x = dsp::shapeHasGain(shape) ? gainDb : 0.0;
        if (i == params::bandParam(b, BandField::Shape)) x = static_cast<int>(shape);
    }
    write(v);
    return b;
}

void BandOps::remove(const Bands& bands) {
    if (bands.empty()) return;
    UndoGroup group(c_, bands.size() == 1 ? "Delete Band" : "Delete Bands");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands)
        for (int f = 0; f < params::kBandFieldCount; ++f) {
            const auto bf = static_cast<BandField>(f);
            v.emplace_back(params::bandParam(b, bf), def(b, bf));  // Used is off by default
        }
    write(v);
}

void BandOps::setEnabled(const Bands& bands, bool on) {
    UndoGroup group(c_, on ? "Enable" : "Disable");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) v.emplace_back(params::bandParam(b, BandField::Enabled), on ? 1.0 : 0.0);
    write(v);
}

void BandOps::invertGain(const Bands& bands) {
    UndoGroup group(c_, "Invert Gain");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) {
        v.emplace_back(params::bandParam(b, BandField::Gain), -value(b, BandField::Gain));
        if (value(b, BandField::DynRange) != 0.0)
            v.emplace_back(params::bandParam(b, BandField::DynRange), -value(b, BandField::DynRange));
    }
    write(v);
}

void BandOps::makeDynamic(const Bands& bands) {
    UndoGroup group(c_, "Make Dynamic");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) {
        v.emplace_back(params::bandParam(b, BandField::DynEnabled), 1.0);
        if (value(b, BandField::DynRange) == 0.0)  // hearable at once (R5)
            v.emplace_back(params::bandParam(b, BandField::DynRange), kDefaultDynamicRangeDb);
    }
    write(v);
}

void BandOps::clearDynamics(const Bands& bands) {
    UndoGroup group(c_, "Clear Dynamics");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands)
        for (auto f : {BandField::DynEnabled, BandField::DynRange, BandField::DynAuto, BandField::Threshold,
                       BandField::Attack, BandField::Release, BandField::Spectral, BandField::SpectralDensity,
                       BandField::SpectralTilt, BandField::ScExternal, BandField::ScMode, BandField::ScLow,
                       BandField::ScHigh, BandField::ScListen})
            v.emplace_back(params::bandParam(b, f), def(b, f));
    write(v);
}

void BandOps::makeSpectral(const Bands& bands) {
    UndoGroup group(c_, "Make Spectral");
    makeDynamic(bands);
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) v.emplace_back(params::bandParam(b, BandField::Spectral), 1.0);
    write(v);
}

void BandOps::clearSpectral(const Bands& bands) {
    UndoGroup group(c_, "Clear Spectral");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) v.emplace_back(params::bandParam(b, BandField::Spectral), 0.0);
    write(v);
}

void BandOps::setShape(const Bands& bands, dsp::Shape shape) {
    UndoGroup group(c_, "Shape");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) {
        v.emplace_back(params::bandParam(b, BandField::Shape), static_cast<int>(shape));
        const auto slope = static_cast<dsp::Slope>(static_cast<int>(value(b, BandField::Slope)));
        if (!dsp::slopeAllowed(shape, slope))  // keep the pair valid (Brickwall is for cuts only)
            v.emplace_back(params::bandParam(b, BandField::Slope), def(b, BandField::Slope));
    }
    write(v);
}

void BandOps::setSlope(const Bands& bands, dsp::Slope slope) {
    UndoGroup group(c_, "Slope");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands)
        if (dsp::slopeAllowed(static_cast<dsp::Shape>(static_cast<int>(value(b, BandField::Shape))), slope))
            v.emplace_back(params::bandParam(b, BandField::Slope), static_cast<int>(slope));
    write(v);
}

void BandOps::setPlacement(const Bands& bands, int placement) {
    UndoGroup group(c_, "Placement");
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (int b : bands) v.emplace_back(params::bandParam(b, BandField::Placement), placement);
    write(v);
}

// Clipboard JSON: {"am-q.bands":1,"bands":[{"used":1,"freq":9.96,...},...]}, every field by the
// part of its string id after "bandNN.", values plain.
std::string BandOps::copy(const Bands& bands) const {
    std::string s = "{\"am-q.bands\":1,\"bands\":[";
    for (size_t k = 0; k < bands.size(); ++k) {
        if (k) s += ',';
        s += '{';
        for (int f = 0; f < params::kBandFieldCount; ++f) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", value(bands[k], static_cast<BandField>(f)));
            if (f) s += ',';
            s += '"' + fieldName(bands[k], f) + "\":" + buf;
        }
        s += '}';
    }
    return s + "]}";
}

std::string BandOps::cut(const Bands& bands) {
    UndoGroup group(c_, "Cut");
    std::string s = copy(bands);
    remove(bands);
    return s;
}

bool BandOps::paste(const std::string& json, const Bands& replace, std::string* message, Bands* created) {
    // Parse: a minimal reader for our own format (objects of "name":number inside "bands").
    if (json.find("\"am-q.bands\"") == std::string::npos) {
        if (message) *message = "The clipboard holds no Am-Q bands.";
        return false;
    }
    std::vector<std::map<std::string, double>> items;
    size_t p = json.find("\"bands\"");
    p = p == std::string::npos ? p : json.find('[', p);
    while (p != std::string::npos) {
        const size_t o = json.find('{', p);
        const size_t e = o == std::string::npos ? o : json.find('}', o);
        if (o == std::string::npos || e == std::string::npos) break;
        std::map<std::string, double> m;
        size_t q = o + 1;
        while (q < e) {
            const size_t k0 = json.find('"', q);
            if (k0 == std::string::npos || k0 >= e) break;
            const size_t k1 = json.find('"', k0 + 1);
            const size_t colon = json.find(':', k1);
            if (k1 == std::string::npos || colon == std::string::npos || colon >= e) break;
            m[json.substr(k0 + 1, k1 - k0 - 1)] = std::strtod(json.c_str() + colon + 1, nullptr);
            q = json.find(',', colon);
            if (q == std::string::npos || q > e) q = e;
            else ++q;
        }
        items.push_back(std::move(m));
        p = e + 1;
        if (json.find('{', p) == std::string::npos) break;
    }
    if (items.empty()) {
        if (message) *message = "The clipboard holds no Am-Q bands.";
        return false;
    }
    // Free bands after the replaced ones are deleted.
    int free = 0;
    for (int b = 0; b < params::kBandCount; ++b)
        if (!used(b) || std::find(replace.begin(), replace.end(), b) != replace.end()) ++free;
    if (free < static_cast<int>(items.size())) {
        if (message) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Not enough free bands: %d needed, %d free.", static_cast<int>(items.size()), free);
            *message = buf;
        }
        return false;  // nothing changed (R10)
    }
    UndoGroup group(c_, replace.empty() ? "Paste" : "Paste Over");
    remove(replace);
    std::vector<std::pair<params::ParamIndex, double>> v;
    int b = 0;
    for (const auto& m : items) {
        while (b < params::kBandCount && used(b)) ++b;
        if (b == params::kBandCount) break;
        for (int f = 0; f < params::kBandFieldCount; ++f) {
            const auto bf = static_cast<BandField>(f);
            const auto it = m.find(fieldName(b, f));
            double x = it != m.end() ? it->second : def(b, bf);
            if (bf == BandField::Used) x = 1.0;
            v.emplace_back(params::bandParam(b, bf), x);
        }
        if (created) created->push_back(b);
        ++b;
        // Mark it used now so the next item takes the next free band.
        write(v);
        v.clear();
    }
    return true;
}

}  // namespace amq::ui
