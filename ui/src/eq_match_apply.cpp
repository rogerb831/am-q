#include "amq/ui/eq_match_apply.hpp"

#include "amq/dsp/conventions.hpp"
#include "amq/params/mappings.hpp"
#include "amq/ui/edit_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace amq::ui {

using params::BandField;

void EqMatchApplier::setFit(std::vector<MatchBand> bands) {
    finish();
    fit_ = std::move(bands);
}

void EqMatchApplier::finish() {
    if (!open_) return;
    c_.endGroup();
    c_.setEditSource(params::ParamSource::Gui);
    open_ = false;
}

MatchBand EqMatchApplier::scaled(const MatchBand& b, double amount) {
    MatchBand s = b;
    amount = std::clamp(amount, 0.0, 1.0);
    s.gainDb = b.gainDb * amount;
    if (b.shape == dsp::Shape::LowCut || b.shape == dsp::Shape::HighCut) {
        const int fitted = static_cast<int>(b.slope), gentle = static_cast<int>(dsp::Slope::Db6);
        s.slope = static_cast<dsp::Slope>(gentle + static_cast<int>(std::lround((fitted - gentle) * amount)));
    }
    return s;
}

int EqMatchApplier::apply(double amount, int count, std::string* message) {
    const auto& t = c_.table();
    if (!open_) {  // one undo step for the apply and every slider move after it (R12, R13)
        c_.beginGroup("EQ Match");
        c_.setEditSource(params::ParamSource::EqMatch);
        open_ = true;
    }
    // The bands to write: a prefix of the fit; cuts drop out at a low Amount.
    std::vector<MatchBand> want;
    for (int i = 0; i < std::min<int>(count, static_cast<int>(fit_.size())); ++i) {
        const auto& b = fit_[static_cast<size_t>(i)];
        const bool cut = b.shape == dsp::Shape::LowCut || b.shape == dsp::Shape::HighCut;
        if (cut && amount < kCutBypassAmount) continue;
        want.push_back(scaled(b, amount));
    }
    // Slots: tagged bands first, then free ones, in band order.
    std::vector<int> slots = tagged_;
    for (int b = 0; b < params::kBandCount && slots.size() < want.size(); ++b)
        if (t.value(params::bandParam(b, BandField::Used)) < 0.5 &&
            std::find(slots.begin(), slots.end(), b) == slots.end())
            slots.push_back(b);
    const size_t n = std::min(want.size(), slots.size());
    std::vector<std::pair<params::ParamIndex, double>> v;
    for (size_t i = 0; i < n; ++i) {
        const int b = slots[i];
        for (int f = 0; f < params::kBandFieldCount; ++f) {
            const auto id = params::bandParam(b, static_cast<BandField>(f));
            v.emplace_back(id, params::specAt(id)->def);
        }
        auto set = [&](BandField f, double x) {
            for (auto& [id, val] : v)
                if (id == params::bandParam(b, f)) val = x;
        };
        const auto& w = want[i];
        set(BandField::Used, 1.0);
        set(BandField::Enabled, 1.0);
        set(BandField::Shape, static_cast<int>(w.shape));
        set(BandField::Slope, static_cast<int>(w.slope));
        set(BandField::Freq, std::log2(std::clamp(w.hz, dsp::kMinBandFrequencyHz, dsp::kMaxBandFrequencyHz)));
        set(BandField::Gain, std::clamp(w.gainDb, -30.0, 30.0));
        set(BandField::Q, params::positionFromQ(w.q));
    }
    // Tagged bands no longer needed go back to their defaults (deleted).
    std::vector<int> keep(slots.begin(), slots.begin() + static_cast<ptrdiff_t>(n));
    for (int b : tagged_)
        if (std::find(keep.begin(), keep.end(), b) == keep.end())
            for (int f = 0; f < params::kBandFieldCount; ++f) {
                const auto id = params::bandParam(b, static_cast<BandField>(f));
                v.emplace_back(id, params::specAt(id)->def);
            }
    if (!v.empty()) {
        std::vector<params::ParamIndex> ids;
        for (const auto& [id, x] : v) ids.push_back(id);
        EditGesture g;
        g.open(c_, ids);
        for (const auto& [id, x] : v) g.set(id, x);
    }
    tagged_ = keep;
    if (message) {
        message->clear();
        if (want.size() > n) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%d fitted band%s not applied: no free bands.", static_cast<int>(want.size() - n),
                          want.size() - n == 1 ? "" : "s");
            *message = buf;
        }
    }
    return static_cast<int>(n);
}

}  // namespace amq::ui
