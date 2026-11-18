#include "amq/ui/graph_model.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/conventions.hpp"
#include "amq/params/mappings.hpp"
#include "amq/params/shape_info.hpp"

#include <algorithm>
#include <cmath>

namespace amq::ui {

using params::BandField;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

float GraphGeometry::xOfHz(double hz) const noexcept {
    const double t = std::log(hz / lo_) / std::log(hi_ / lo_);
    return kLeft + static_cast<float>(t) * (w_ - kLeft - kRight);
}

double GraphGeometry::hzOfX(float x) const noexcept {
    const double t = (x - kLeft) / std::max(1.0f, w_ - kLeft - kRight);
    return lo_ * std::pow(hi_ / lo_, t);
}

void GraphGeometry::setView(double loHz, double hiHz) noexcept {
    lo_ = std::clamp(loHz, kMinHz, kMaxHz / 2.0);
    hi_ = std::clamp(hiHz, lo_ * 2.0, kMaxHz);  // at least an octave on screen
}

void GraphGeometry::zoomAt(float x, double factor) noexcept {
    const double at = std::log2(hzOfX(x));
    const double lo = at - (at - std::log2(lo_)) * factor, hi = at + (std::log2(hi_) - at) * factor;
    double l = std::exp2(lo), h = std::exp2(hi);
    if (l < kMinHz) h *= kMinHz / l, l = kMinHz;  // keep the span, slide it back inside
    if (h > kMaxHz) l *= kMaxHz / h, h = kMaxHz;
    setView(l, h);
}

double GraphGeometry::octavesPerPixel() const noexcept {
    return std::log2(hi_ / lo_) / std::max(1.0f, w_ - kLeft - kRight);
}

float GraphGeometry::yOfDb(double db) const noexcept {
    const float mid = kTop + 0.5f * (h_ - kTop - kBottom);
    return mid - static_cast<float>(db / rangeDb_) * 0.5f * (h_ - kTop - kBottom);
}

double GraphGeometry::dbOfY(float y) const noexcept {
    const float mid = kTop + 0.5f * (h_ - kTop - kBottom);
    return (mid - y) / std::max(1.0f, 0.5f * (h_ - kTop - kBottom)) * rangeDb_;
}

// ---------------------------------------------------------------------------
// Response curve
// ---------------------------------------------------------------------------

void ResponseCurve::setFrequencies(std::vector<double> hz) {
    hz_ = std::move(hz);
    for (auto& b : band_) b.assign(hz_.size(), 0.0);
    combined_.assign(hz_.size(), 0.0);
    markAllDirty();
}

void ResponseCurve::setSettled(int band, const std::vector<double>& db) {
    const auto b = static_cast<size_t>(band);
    if (db.size() != hz_.size() || dirty_[b] || !audible_[b]) return;
    band_[b] = db;
    settled_[b] = true;
    recombine_ = true;
}

bool ResponseCurve::update(const params::ParameterTable& t) {
    const double scale = t.value(params::globalParam(params::GlobalParam::GainScale)) / 100.0;
    if (scale != lastScale_) {
        lastScale_ = scale;
        markAllDirty();
    }
    bool any = false;
    for (int b = 0; b < params::kBandCount; ++b) {
        if (!dirty_[static_cast<size_t>(b)]) continue;
        dirty_[static_cast<size_t>(b)] = false;
        any = true;
        auto& out = band_[static_cast<size_t>(b)];
        const bool used = t.value(params::bandParam(b, BandField::Used)) >= 0.5;
        const bool enabled = t.value(params::bandParam(b, BandField::Enabled)) >= 0.5;
        audible_[static_cast<size_t>(b)] = used && enabled;
        if (!audible_[static_cast<size_t>(b)]) {
            std::fill(out.begin(), out.end(), 0.0);
            continue;
        }
        dsp::BandSpec s;
        s.shape = static_cast<dsp::Shape>(static_cast<int>(t.value(params::bandParam(b, BandField::Shape))));
        s.slope = static_cast<dsp::Slope>(static_cast<int>(t.value(params::bandParam(b, BandField::Slope))));
        s.frequencyHz = std::exp2(t.value(params::bandParam(b, BandField::Freq)));
        s.gainDb = std::clamp(t.value(params::bandParam(b, BandField::Gain)) * scale, -30.0, 30.0);  // area 01 R13
        s.q = params::qFromPosition(t.value(params::bandParam(b, BandField::Q)));
        const auto proto = dsp::designAnalogPrototype(s);
        for (size_t i = 0; i < hz_.size(); ++i)
            out[i] = proto.supported ? dsp::linearToDb(std::max(std::abs(proto.evaluateHz(hz_[i])), 1e-12)) : 0.0;
    }
    if (any || recombine_) {
        recombine_ = false;
        any = true;
        std::fill(combined_.begin(), combined_.end(), 0.0);
        for (int b = 0; b < params::kBandCount; ++b)
            if (audible_[static_cast<size_t>(b)])
                for (size_t i = 0; i < hz_.size(); ++i) combined_[i] += band_[static_cast<size_t>(b)][i];
    }
    return any;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

NodeInfo GraphInteraction::node(int b) const {
    NodeInfo n;
    n.used = value(params::bandParam(b, BandField::Used)) >= 0.5;
    n.enabled = value(params::bandParam(b, BandField::Enabled)) >= 0.5;
    const auto shape = static_cast<dsp::Shape>(static_cast<int>(value(params::bandParam(b, BandField::Shape))));
    n.hasGain = params::shapeInfo(shape).hasGain;
    n.hz = std::exp2(value(params::bandParam(b, BandField::Freq)));
    n.gainDb = value(params::bandParam(b, BandField::Gain));
    n.rangeDb = value(params::bandParam(b, BandField::DynRange));
    n.dynamic = value(params::bandParam(b, BandField::DynEnabled)) >= 0.5;
    n.soloed = soloed(b);
    n.selected = isSelected(b);
    n.focused = b == focused_;
    n.x = g_.xOfHz(n.hz);
    n.y = g_.yOfDb(n.hasGain ? n.gainDb : 0.0);
    n.hasRangeHandle = n.used && n.hasGain && (n.dynamic || n.rangeDb != 0.0);
    n.rangeY = g_.yOfDb(n.gainDb + n.rangeDb);
    return n;
}

bool GraphInteraction::soloed(int b) const { return value(params::bandParam(b, BandField::Solo)) >= 0.5; }

bool GraphInteraction::isSelected(int b) const noexcept {
    return std::find(selection_.begin(), selection_.end(), b) != selection_.end();
}

int GraphInteraction::hitTest(float x, float y) const {
    for (int b = params::kBandCount - 1; b >= 0; --b) {
        const auto n = node(b);
        if (!n.used) continue;
        const float dx = x - n.x, dy = y - n.y;
        if (dx * dx + dy * dy <= kNodeRadiusPx * kNodeRadiusPx) return b;
    }
    return -1;
}

int GraphInteraction::hitRangeHandle(float x, float y) const {
    for (int b = params::kBandCount - 1; b >= 0; --b) {
        const auto n = node(b);
        if (!n.hasRangeHandle || n.rangeDb == 0.0) continue;
        const float dx = x - n.x, dy = y - n.rangeY;
        if (dx * dx + dy * dy <= kHandleRadiusPx * kHandleRadiusPx) return b;
    }
    return -1;
}

void GraphInteraction::changed() {
    if (onSelectionChanged) onSelectionChanged();
}

void GraphInteraction::setSelection(std::vector<int> bands, int focus) {
    std::sort(bands.begin(), bands.end());
    bands.erase(std::unique(bands.begin(), bands.end()), bands.end());
    bands.erase(std::remove_if(bands.begin(), bands.end(), [](int b) { return b < 0 || b >= params::kBandCount; }),
                bands.end());
    const int f = focus == -2 ? (bands.empty() ? -1 : (isSelected(focused_) && std::find(bands.begin(), bands.end(), focused_) != bands.end() ? focused_ : bands.back())) : focus;
    if (bands == selection_ && f == focused_) return;
    selection_ = std::move(bands);
    focused_ = f;
    changed();
}

void GraphInteraction::selectAll() {
    std::vector<int> all;
    for (int b = 0; b < params::kBandCount; ++b)
        if (node(b).used) all.push_back(b);
    setSelection(all, all.empty() ? -1 : (focused_ >= 0 ? focused_ : all.front()));
}

void GraphInteraction::clearSelection() { setSelection({}, -1); }

void GraphInteraction::focusNext(int direction) {
    std::vector<int> used;
    for (int b = 0; b < params::kBandCount; ++b)
        if (node(b).used) used.push_back(b);
    if (used.empty()) return;
    // Order by frequency, as the bands appear on screen.
    std::sort(used.begin(), used.end(), [&](int a, int b) { return node(a).hz < node(b).hz; });
    auto it = std::find(used.begin(), used.end(), focused_);
    int k = it == used.end() ? (direction > 0 ? 0 : static_cast<int>(used.size()) - 1)
                             : static_cast<int>(it - used.begin()) + (direction > 0 ? 1 : -1);
    k = (k + static_cast<int>(used.size())) % static_cast<int>(used.size());
    setSelection({used[static_cast<size_t>(k)]}, used[static_cast<size_t>(k)]);
}

std::vector<int> GraphInteraction::contextTargets(float x, float y) {
    const int hit = hitTest(x, y);
    if (hit >= 0 && !isSelected(hit)) setSelection({hit}, hit);
    return hit >= 0 || !selection_.empty() ? selection_ : std::vector<int>{};
}

std::array<float, 4> GraphInteraction::lassoRect() const noexcept {
    return {std::min(downX_, curX_), std::min(downY_, curY_), std::max(downX_, curX_), std::max(downY_, curY_)};
}

double GraphInteraction::soloDragDb() const { return value(params::globalParam(params::GlobalParam::SoloGain)); }

int GraphInteraction::createBandAt(float x, float y) {
    int b = -1;
    for (int i = 0; i < params::kBandCount; ++i)
        if (value(params::bandParam(i, BandField::Used)) < 0.5) {
            b = i;
            break;
        }
    if (b < 0) return -1;
    // Every field of the band at its default, then Used, Frequency and Gain from the click,
    // all in one gesture (area 13 R19; area 21 R8).
    std::vector<params::ParamIndex> ids;
    for (int f = 0; f < params::kBandFieldCount; ++f) ids.push_back(params::bandParam(b, static_cast<BandField>(f)));
    EditGesture g;
    g.open(c_, ids);
    for (auto id : ids) g.set(id, params::specAt(id)->def);
    g.set(params::bandParam(b, BandField::Used), 1.0);
    const double hz = std::clamp(g_.hzOfX(x), dsp::kMinBandFrequencyHz, dsp::kMaxBandFrequencyHz);
    g.set(params::bandParam(b, BandField::Freq), std::log2(hz));
    g.set(params::bandParam(b, BandField::Gain), std::clamp(g_.dbOfY(y), -30.0, 30.0));
    g.close();
    setSelection({b}, b);
    return b;
}

void GraphInteraction::pointerDown(float x, float y, Button button, Modifiers m, int clickCount) {
    gesture_.close();
    mode_ = Mode::None;
    pressed_ = -1;
    items_.clear();
    downX_ = curX_ = x, downY_ = curY_ = y;
    axis_ = -1;
    if (button != Button::Left) return;  // the editor opens menus with contextTargets()
    // The range handle first: it sits above or below its node.
    if (const int h = hitRangeHandle(x, y); h >= 0) {
        if (m.alt) {  // toggle Dynamics Enabled, one gesture (area 13 R7)
            const auto id = params::bandParam(h, BandField::DynEnabled);
            EditGesture g(c_, {id});
            g.set(id, value(id) >= 0.5 ? 0.0 : 1.0);
            return;
        }
        pressed_ = h;
        range0_ = value(params::bandParam(h, BandField::DynRange));
        mode_ = Mode::Pending;
        pendingMode_ = Mode::Range;
        return;
    }
    const int hit = hitTest(x, y);
    if (hit < 0) {
        if (clickCount >= 2) {
            createBandAt(x, y);
            return;
        }
        lassoAdd_ = m.shift;
        if (!m.shift) clearSelection();
        mode_ = Mode::Pending;
        pendingMode_ = Mode::Lasso;
        return;
    }
    if (m.alt && !m.command && !m.shift) {  // toggle Enabled, one gesture (area 13 R7)
        setSelection({hit}, hit);
        const auto id = params::bandParam(hit, BandField::Enabled);
        EditGesture g(c_, {id});
        g.set(id, value(id) >= 0.5 ? 0.0 : 1.0);
        pressed_ = hit;  // an Alt-drag after this changes Q
        pendingAltQ_ = true;
        mode_ = Mode::None;
        return;
    }
    pendingAltQ_ = false;
    clickSelectsOnly_ = false;
    if (m.command) {
        auto s = selection_;
        if (isSelected(hit)) s.erase(std::find(s.begin(), s.end(), hit));
        else s.push_back(hit);
        setSelection(s, isSelected(hit) ? (s.empty() ? -1 : s.back()) : hit);
        if (!isSelected(hit)) return;
    } else if (m.shift) {
        auto s = selection_;
        s.push_back(hit);
        setSelection(s, hit);
    } else if (!isSelected(hit)) {
        setSelection({hit}, hit);
    } else {
        setSelection(selection_, hit);
        clickSelectsOnly_ = selection_.size() > 1;
    }
    pressed_ = hit;
    mode_ = Mode::Pending;
    pendingMode_ = soloed(hit) ? Mode::Solo : Mode::Nodes;
}

void GraphInteraction::beginDrag(Modifiers m) {
    std::vector<params::ParamIndex> ids;
    undoValues_.clear();
    auto add = [&](params::ParamIndex i) {
        ids.push_back(i);
        undoValues_.emplace_back(i, value(i));
    };
    if (pendingMode_ == Mode::Range) {
        add(params::bandParam(pressed_, BandField::DynRange));
    } else if (pendingMode_ == Mode::Nodes || pendingMode_ == Mode::Solo) {
        const auto drag = isSelected(pressed_) ? selection_ : std::vector<int>{pressed_};
        for (int b : drag) {
            Item it;
            it.band = b;
            it.hasGain = node(b).hasGain;
            it.f0 = value(params::bandParam(b, BandField::Freq));
            it.g0 = value(params::bandParam(b, BandField::Gain));
            it.q0 = value(params::bandParam(b, BandField::Q));
            items_.push_back(it);
            add(params::bandParam(b, BandField::Freq));
            if (pendingMode_ == Mode::Solo) continue;
            const bool q = !it.hasGain || m.alt;
            add(params::bandParam(b, q ? BandField::Q : BandField::Gain));
            if (!q && gainQCoupling) add(params::bandParam(b, BandField::Q));
        }
        if (pendingMode_ == Mode::Solo) {
            solo0_ = value(params::globalParam(params::GlobalParam::SoloGain));
            add(params::globalParam(params::GlobalParam::SoloGain));
        }
    }
    mode_ = pendingMode_;
    if (!ids.empty()) gesture_.open(c_, ids);
}

void GraphInteraction::pointerDrag(float x, float y, Modifiers m) {
    curX_ = x, curY_ = y;
    if (mode_ == Mode::None && pendingAltQ_ && pressed_ >= 0) {  // Alt-drag after Alt-click: Q
        const float dx = x - downX_, dy = y - downY_;
        if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) return;
        mode_ = Mode::Pending;
        pendingMode_ = Mode::Nodes;
        setSelection({pressed_}, pressed_);
        m.alt = true;
    }
    if (mode_ == Mode::Pending) {
        const float dx = x - downX_, dy = y - downY_;
        if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) return;  // still a click (area 13 R2)
        clickSelectsOnly_ = false;
        beginDrag(m);
    }
    if (mode_ == Mode::Lasso || mode_ == Mode::None || mode_ == Mode::Pending) return;
    double dx = x - downX_, dy = y - downY_;
    if (m.command) {  // axis lock: the axis that dominated when the lock started
        if (axis_ < 0) axis_ = std::abs(dx) >= std::abs(dy) ? 0 : 1;
        if (axis_ == 0) dy = 0;
        else dx = 0;
    } else {
        axis_ = -1;
    }
    if (m.shift) dx *= kFineFactor, dy *= kFineFactor;
    const double dOct = dx * g_.octavesPerPixel();
    const double dDb = g_.dbOfY(static_cast<float>(downY_ + dy)) - g_.dbOfY(downY_);
    if (mode_ == Mode::Range) {
        gesture_.set(params::bandParam(pressed_, BandField::DynRange), std::clamp(range0_ + dDb, -30.0, 30.0));
    } else {
        for (const auto& it : items_) {
            const double lo = std::log2(dsp::kMinBandFrequencyHz), hi = std::log2(dsp::kMaxBandFrequencyHz);
            gesture_.set(params::bandParam(it.band, BandField::Freq), std::clamp(it.f0 + dOct, lo, hi));
            if (mode_ == Mode::Solo) continue;
            if (!it.hasGain || m.alt) {
                gesture_.set(params::bandParam(it.band, BandField::Q), it.q0 - dy * kDragQPerPx);
            } else {
                const double g = std::clamp(it.g0 + dDb, -30.0, 30.0);
                gesture_.set(params::bandParam(it.band, BandField::Gain), g);
                if (gainQCoupling) {
                    const double q = params::qFromPosition(it.q0) * std::exp2((std::abs(g) - std::abs(it.g0)) / 12.0);
                    gesture_.set(params::bandParam(it.band, BandField::Q), params::positionFromQ(q));
                }
            }
        }
        if (mode_ == Mode::Solo)
            gesture_.set(params::globalParam(params::GlobalParam::SoloGain), std::clamp(solo0_ + dDb, -20.0, 20.0));
    }
    c_.dispatchNotifications();
}

void GraphInteraction::pointerUp(float x, float y, Modifiers m) {
    (void)m;
    curX_ = x, curY_ = y;
    if (mode_ == Mode::Lasso) {  // select the nodes inside the rectangle
        const auto r = lassoRect();
        auto s = lassoAdd_ ? selection_ : std::vector<int>{};
        for (int b = 0; b < params::kBandCount; ++b) {
            const auto n = node(b);
            if (n.used && n.x >= r[0] && n.x <= r[2] && n.y >= r[1] && n.y <= r[3]) s.push_back(b);
        }
        setSelection(s);
    } else if (mode_ == Mode::Pending && clickSelectsOnly_ && pressed_ >= 0) {
        setSelection({pressed_}, pressed_);  // a click inside a multi-selection keeps only that band
    }
    gesture_.close();
    mode_ = Mode::None;
    pressed_ = -1;
    pendingAltQ_ = false;
    items_.clear();
}

void GraphInteraction::cancelDrag() {
    if (gesture_.isOpen())
        for (const auto& [i, v] : undoValues_) gesture_.set(i, v);
    gesture_.close();
    mode_ = Mode::None;
    pressed_ = -1;
    items_.clear();
}

void GraphInteraction::wheel(float x, float y, double delta, Modifiers m) {
    if (delta == 0.0) return;
    const int hit = hitTest(x, y);
    std::vector<int> targets = hit >= 0 ? (isSelected(hit) ? selection_ : std::vector<int>{hit}) : selection_;
    if (targets.empty() && focused_ >= 0 && hit < 0) targets = {focused_};
    if (targets.empty()) {  // nothing to act on: horizontal zoom around the pointer
        g_.zoomAt(x, std::pow(kZoomPerWheel, delta));
        return;
    }
    BandField f = BandField::Q;
    if (m.command && m.alt) f = BandField::Slope;
    else if (m.command) f = BandField::Gain;
    else if (m.alt) f = BandField::DynRange;
    const double fine = m.shift ? kFineFactor : 1.0;  // R12: the fine modifier also scales wheel steps
    std::vector<params::ParamIndex> ids;
    for (int b : targets) ids.push_back(params::bandParam(b, f));
    EditGesture g;
    g.open(c_, ids);
    for (int b : targets) {
        const auto id = params::bandParam(b, f);
        const double v = value(id);
        switch (f) {
            case BandField::Q: g.set(id, v + delta * kWheelQStep * fine); break;
            case BandField::Gain:
                if (node(b).hasGain) g.set(id, v + delta * kWheelGainDb * fine);
                break;
            case BandField::DynRange: g.set(id, v + delta * kWheelGainDb * fine); break;
            default: {  // Slope: the next slope the shape allows, in the wheel's direction
                const auto shape = static_cast<dsp::Shape>(static_cast<int>(value(params::bandParam(b, BandField::Shape))));
                int s = static_cast<int>(v);
                const int dir = delta > 0 ? 1 : -1;
                for (int k = s + dir; k >= 0 && k < dsp::kSlopeCount; k += dir)
                    if (dsp::slopeAllowed(shape, static_cast<dsp::Slope>(k))) {
                        s = k;
                        break;
                    }
                g.set(id, s);
                break;
            }
        }
    }
}

}  // namespace amq::ui
