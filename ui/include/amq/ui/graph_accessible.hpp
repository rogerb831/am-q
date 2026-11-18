#pragma once
// Accessible controls for the response graph (area 15 R12): one per band node, and the graph group
// that lists the used bands in frequency order, the same order as Tab (GraphInteraction::focusNext).
// The focus they report and take is the graph's focused band, so the keys and assistive tools
// share one focus.

#include "amq/ui/accessible.h"
#include "amq/ui/graph_model.hpp"

#include <array>
#include <memory>

namespace amq::ui {

class BandNodeAccessible : public AccessibleControl {
public:
    BandNodeAccessible(EditController& c, GraphInteraction& m, int band) : c_(c), m_(m), band_(band) {}
    std::string accessibleName() const override { return "Band " + std::to_string(band_ + 1); }
    AccessibleRole accessibleRole() const override { return AccessibleRole::GraphNode; }
    std::string accessibleValueText() const override;
    double accessibleValueNormalised() const override;
    std::vector<AccessibleAction> accessibleActions() const override {
        return {AccessibleAction::Press, AccessibleAction::Increment, AccessibleAction::Decrement, AccessibleAction::SetValue};
    }
    // Press selects; Increment/Decrement nudge gain by 1 dB; SetValue sets the
    // normalised frequency. All through gestures.
    bool performAccessibleAction(AccessibleAction a, double value) override;
    bool accessibleVisible() const override { return m_.node(band_).used; }
    bool accessibleFocused() const override { return m_.focused() == band_; }
    bool setAccessibleFocus() override;
    [[nodiscard]] int band() const noexcept { return band_; }

private:
    EditController& c_;
    GraphInteraction& m_;
    int band_;
};

class GraphAccessible : public AccessibleControl {
public:
    GraphAccessible(EditController& c, GraphInteraction& m);
    std::string accessibleName() const override { return "Response graph"; }
    AccessibleRole accessibleRole() const override { return AccessibleRole::Group; }
    std::vector<AccessibleAction> accessibleActions() const override { return {}; }
    bool performAccessibleAction(AccessibleAction, double) override { return false; }
    // The used bands, ordered by frequency (Tab order).
    std::vector<AccessibleControl*> accessibleChildren() const override;

private:
    GraphInteraction& m_;
    std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount> nodes_;
};

}  // namespace amq::ui
