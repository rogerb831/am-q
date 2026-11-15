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
    const double t = std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz);
    return kLeft + static_cast<float>(t) * (w_ - kLeft - kRight);
}

double GraphGeometry::hzOfX(float x) const noexcept {
    const double t = (x - kLeft) / std::max(1.0f, w_ - kLeft - kRight);
    return kMinHz * std::pow(kMaxHz / kMinHz, t);
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
    if (any) {
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
    n.x = g_.xOfHz(n.hz);
    n.y = g_.yOfDb(n.hasGain ? n.gainDb : 0.0);
    return n;
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

void GraphInteraction::select(int band) {
    if (band == selected_) return;
    selected_ = band;
    if (onSelectionChanged) onSelectionChanged();
}

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
    select(b);
    return b;
}

void GraphInteraction::pointerDown(float x, float y, Button button, Modifiers m, int clickCount) {
    gesture_.close();
    pending_ = false;
    pressed_ = -1;
    if (button != Button::Left) return;  // right-click menus arrive with area 14
    const int hit = hitTest(x, y);
    if (hit < 0) {
        if (clickCount >= 2) createBandAt(x, y);
        else select(-1);
        return;
    }
    select(hit);
    if (m.alt) {  // toggle Enabled, one gesture (area 13 R7)
        const auto id = params::bandParam(hit, BandField::Enabled);
        EditGesture g(c_, {id});
        g.set(id, value(id) >= 0.5 ? 0.0 : 1.0);
        return;
    }
    const auto n = node(hit);
    pressed_ = hit;
    pending_ = true;
    downX_ = x, downY_ = y;
    grabDx_ = x - n.x, grabDy_ = y - n.y;
    startQ_ = value(params::bandParam(hit, BandField::Q));
}

void GraphInteraction::pointerDrag(float x, float y, Modifiers) {
    if (pressed_ < 0) return;
    const int b = pressed_;
    const bool hasGain = node(b).hasGain;
    const auto fId = params::bandParam(b, BandField::Freq);
    const auto vId = params::bandParam(b, hasGain ? BandField::Gain : BandField::Q);
    if (pending_) {
        const float dx = x - downX_, dy = y - downY_;
        if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) return;  // still a click (area 13 R2)
        pending_ = false;
        gesture_.open(c_, {fId, vId});
    }
    const double hz = std::clamp(g_.hzOfX(x - grabDx_), dsp::kMinBandFrequencyHz, dsp::kMaxBandFrequencyHz);
    gesture_.set(fId, std::log2(hz));
    if (hasGain) gesture_.set(vId, g_.dbOfY(y - grabDy_));
    else gesture_.set(vId, startQ_ + (downY_ - y) * kDragQPerPx);
    c_.dispatchNotifications();
}

void GraphInteraction::pointerUp(float, float, Modifiers) {
    gesture_.close();
    pending_ = false;
    pressed_ = -1;
}

void GraphInteraction::wheel(float x, float y, double delta, Modifiers) {
    int b = hitTest(x, y);
    if (b < 0) b = selected_;
    if (b < 0 || delta == 0.0) return;
    const auto id = params::bandParam(b, BandField::Q);
    EditGesture g(c_, {id});
    g.set(id, value(id) + delta * kWheelQStep);
}

}  // namespace amq::ui
