#include "amq/ui/graph_accessible.hpp"

#include "amq/params/mappings.hpp"

#include <algorithm>

namespace amq::ui {

using params::BandField;

std::string BandNodeAccessible::accessibleValueText() const {
    const auto f = params::bandParam(band_, BandField::Freq);
    const auto g = params::bandParam(band_, BandField::Gain);
    return params::formatPlain(*params::specAt(f), c_.table().value(f)) + ", " +
           params::formatPlain(*params::specAt(g), c_.table().value(g));
}

double BandNodeAccessible::accessibleValueNormalised() const {
    const auto f = params::bandParam(band_, BandField::Freq);
    return params::toNormalised(*params::specAt(f), c_.table().value(f));
}

bool BandNodeAccessible::performAccessibleAction(AccessibleAction a, double value) {
    if (!accessibleVisible()) return false;
    const auto g = params::bandParam(band_, BandField::Gain);
    const auto f = params::bandParam(band_, BandField::Freq);
    switch (a) {
        case AccessibleAction::Press: m_.setSelection({band_}, band_); return true;
        case AccessibleAction::Increment:
        case AccessibleAction::Decrement: {
            EditGesture e(c_, {g});
            e.set(g, c_.table().value(g) + (a == AccessibleAction::Increment ? 1.0 : -1.0));
            return true;
        }
        case AccessibleAction::SetValue: {
            EditGesture e(c_, {f});
            e.set(f, params::fromNormalised(*params::specAt(f), value));
            return true;
        }
        default: return false;
    }
}

bool BandNodeAccessible::setAccessibleFocus() {
    if (!accessibleFocusable()) return false;
    m_.setSelection({band_}, band_);  // the graph's focus: selecting focuses, as a click does
    return true;
}

GraphAccessible::GraphAccessible(EditController& c, GraphInteraction& m) : m_(m) {
    for (int b = 0; b < params::kBandCount; ++b) nodes_[static_cast<size_t>(b)] = std::make_unique<BandNodeAccessible>(c, m, b);
}

std::vector<AccessibleControl*> GraphAccessible::accessibleChildren() const {
    std::vector<BandNodeAccessible*> used;
    for (const auto& n : nodes_)
        if (n->accessibleVisible()) used.push_back(n.get());
    std::stable_sort(used.begin(), used.end(), [&](const BandNodeAccessible* a, const BandNodeAccessible* b) {
        return m_.node(a->band()).hz < m_.node(b->band()).hz;
    });
    return {used.begin(), used.end()};
}

}  // namespace amq::ui
