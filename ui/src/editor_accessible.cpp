#include "amq/ui/editor_accessible.hpp"

#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cmath>

namespace amq::ui {

using params::BandField;
using params::GlobalParam;

// ---------------------------------------------------------------------------
// ParamAccessible
// ---------------------------------------------------------------------------

ParamAccessible::ParamAccessible(EditController& c, EditorFocus& f, params::ParamIndex id, std::string name)
    : c_(c), focus_(f), id_(id), name_(std::move(name)) {}

AccessibleRole ParamAccessible::accessibleRole() const {
    switch (params::specAt(id_)->type) {
        case params::ParamType::Toggle: return AccessibleRole::Toggle;
        case params::ParamType::Choice: return AccessibleRole::Choice;
        default: return AccessibleRole::Slider;
    }
}

std::string ParamAccessible::accessibleValueText() const { return params::displayText(c_.table(), id_); }

double ParamAccessible::accessibleValueNormalised() const {
    return params::toNormalised(*params::specAt(id_), c_.table().value(id_));
}

std::vector<AccessibleAction> ParamAccessible::accessibleActions() const {
    if (accessibleRole() == AccessibleRole::Toggle) return {AccessibleAction::Press, AccessibleAction::SetValue};
    return {AccessibleAction::Press, AccessibleAction::Increment, AccessibleAction::Decrement, AccessibleAction::SetValue};
}

bool ParamAccessible::performAccessibleAction(AccessibleAction a, double value) {
    if (!accessibleEnabled() || !accessibleVisible()) return false;
    const auto& spec = *params::specAt(id_);
    const double v = c_.table().value(id_);
    double next = v;
    switch (a) {
        case AccessibleAction::Press:
            if (spec.type == params::ParamType::Toggle) next = v >= 0.5 ? 0.0 : 1.0;
            else if (spec.type == params::ParamType::Choice) next = v + 1.0 > spec.max ? spec.min : v + 1.0;
            else return false;
            break;
        case AccessibleAction::Increment:
        case AccessibleAction::Decrement: {
            const double dir = a == AccessibleAction::Increment ? 1.0 : -1.0;
            if (spec.type == params::ParamType::Continuous)
                next = params::fromNormalised(spec, std::clamp(params::toNormalised(spec, v) + dir * 0.01, 0.0, 1.0));
            else
                next = std::clamp(v + dir, spec.min, spec.max);
            break;
        }
        case AccessibleAction::SetValue: next = params::fromNormalised(spec, std::clamp(value, 0.0, 1.0)); break;
        default: return false;
    }
    EditGesture g(c_, {id_});
    g.set(id_, next);
    return true;
}

bool ParamAccessible::setAccessibleFocus() {
    if (!accessibleFocusable()) return false;
    focus_.control = this;
    return true;
}

// ---------------------------------------------------------------------------
// CommandAccessible
// ---------------------------------------------------------------------------

bool CommandAccessible::performAccessibleAction(AccessibleAction a, double) {
    if (a != AccessibleAction::Press || !accessibleEnabled() || !run_) return false;
    run_();
    return true;
}

bool CommandAccessible::setAccessibleFocus() {
    if (!accessibleFocusable()) return false;
    focus_.control = this;
    return true;
}

// ---------------------------------------------------------------------------
// EditorAccessible
// ---------------------------------------------------------------------------

EditorAccessible::EditorAccessible(EditController& c, GraphInteraction& m, BandOps& ops) : c_(c), m_(m), ops_(ops) {
    for (int b = 0; b < params::kBandCount; ++b) nodes_[static_cast<size_t>(b)] = std::make_unique<FocusingBandNode>(c, m, focus_, b);
    for (const char* n : {"Undo", "Redo", "EQ Match", "Settings", "Help"})
        commands_[n] = std::make_unique<CommandAccessible>(focus_, n, std::function<void()>{}, [] { return false; });
    deleteBand_ = std::make_unique<CommandAccessible>(
        focus_, "Delete band",
        [this] {
            const int b = m_.focused();
            if (b >= 0) ops_.remove({b}), m_.clearSelection();
        },
        [this] { return m_.focused() >= 0; });
    top_ = std::make_unique<GroupAccessible>("Top bar", [this] {
        std::vector<AccessibleControl*> v;
        for (const char* n : {"Undo", "Redo", "EQ Match", "Settings", "Help"}) v.push_back(commands_[n].get());
        return v;
    });
    graph_ = std::make_unique<GroupAccessible>("Response graph", [this] { return graphChildren(); });
    panel_ = std::make_unique<GroupAccessible>("Band panel", [this] { return panelChildren(); });
    status_ = std::make_unique<GroupAccessible>("Status bar", [this] {
        auto g = [](GlobalParam p) { return params::globalParam(p); };
        return std::vector<AccessibleControl*>{
            param(g(GlobalParam::Mode), "Processing Mode"),   param(g(GlobalParam::AnalyzerFreeze), "Analyzer Freeze"),
            param(g(GlobalParam::AutoGain), "Auto Gain"),     param(g(GlobalParam::GainScale), "Gain Scale"),
            param(g(GlobalParam::PanMode), "Pan Mode"),       param(g(GlobalParam::OutputLevel), "Output Level"),
            param(g(GlobalParam::Invert), "Invert Phase"),    param(g(GlobalParam::Bypass), "Bypass")};
    });
    root_ = std::make_unique<GroupAccessible>("Am-Q", [this] {
        return std::vector<AccessibleControl*>{top_.get(), graph_.get(), panel_.get(), status_.get()};
    });
}

void EditorAccessible::setCommand(const std::string& name, std::function<void()> run, std::function<bool()> enabled) {
    commands_[name] = std::make_unique<CommandAccessible>(focus_, name, std::move(run), std::move(enabled));
}

ParamAccessible* EditorAccessible::param(params::ParamIndex id, const std::string& name) {
    auto& p = params_[id];
    if (!p) p = std::make_unique<ParamAccessible>(c_, focus_, id, name);
    return p.get();
}

std::vector<AccessibleControl*> EditorAccessible::graphChildren() const {
    std::vector<FocusingBandNode*> used;
    for (const auto& n : nodes_)
        if (n->accessibleVisible()) used.push_back(n.get());
    std::stable_sort(used.begin(), used.end(),
                     [&](const FocusingBandNode* a, const FocusingBandNode* b) { return m_.node(a->band()).hz < m_.node(b->band()).hz; });
    return {used.begin(), used.end()};
}

// The focused band's panel controls, from the panel model: visible controls only, named
// "Band N <label>", greyed (disabled) as the panel greys them.
std::vector<AccessibleControl*> EditorAccessible::panelChildren() {
    std::vector<AccessibleControl*> v;
    const int b = m_.focused();
    if (b < 0 || c_.table().value(params::bandParam(b, BandField::Used)) < 0.5) return v;
    const std::string prefix = "Band " + std::to_string(b + 1) + " ";
    for (const auto& pc : bandPanel(c_.table(), b)) {
        if (!pc.visible) continue;
        if (pc.kind == ControlKind::Button) {
            if (pc.button == PanelButton::Delete) v.push_back(deleteBand_.get());
            continue;
        }
        auto* p = param(pc.id, prefix + pc.label);
        const auto id = pc.id;
        const int band = b;
        p->enabled_ = [this, id, band] {
            for (const auto& q : bandPanel(c_.table(), band))
                if (q.id == id) return q.enabled;
            return false;
        };
        v.push_back(p);
    }
    return v;
}

bool EditorAccessible::panelHasFocus() {
    if (!focus_.control) return false;
    for (auto* c : panelChildren())
        if (c == focus_.control) return true;
    return false;
}

bool EditorAccessible::panelKey(PanelKey k, bool fine) {
    const auto kids = panelChildren();
    std::vector<AccessibleControl*> order;
    for (auto* c : kids)
        if (c->accessibleFocusable()) order.push_back(c);
    if (!panelHasFocus()) {
        if (k != PanelKey::Enter || m_.focused() < 0 || order.empty()) return false;
        return order.front()->setAccessibleFocus();  // into the panel
    }
    auto* f = const_cast<AccessibleControl*>(focus_.control);
    switch (k) {
        case PanelKey::Escape: focus_.control = nullptr; return true;  // back to the band
        case PanelKey::Next:
        case PanelKey::Previous: {
            auto it = std::find(order.begin(), order.end(), f);
            const int n = static_cast<int>(order.size());
            int i = it == order.end() ? 0 : static_cast<int>(it - order.begin());
            i = ((i + (k == PanelKey::Next ? 1 : -1)) % n + n) % n;
            return order[static_cast<size_t>(i)]->setAccessibleFocus();
        }
        case PanelKey::Enter: f->performAccessibleAction(AccessibleAction::Press, 0.0); return true;
        case PanelKey::Up:
        case PanelKey::Down: {
            const bool up = k == PanelKey::Up;
            auto* p = dynamic_cast<ParamAccessible*>(f);
            if (p && fine && params::specAt(p->id())->type == params::ParamType::Continuous) {
                const double n = std::clamp(p->accessibleValueNormalised() + (up ? 0.001 : -0.001), 0.0, 1.0);
                p->performAccessibleAction(AccessibleAction::SetValue, n);
            } else {
                f->performAccessibleAction(up ? AccessibleAction::Increment : AccessibleAction::Decrement, 0.0);
            }
            return true;
        }
    }
    return false;
}

long EditorAccessible::focusedParam() const noexcept {
    if (const auto* p = dynamic_cast<const ParamAccessible*>(focus_.control)) return static_cast<long>(p->id());
    return -1;
}

std::string EditorAccessible::focusedName() const { return focus_.control ? focus_.control->accessibleName() : std::string(); }

}  // namespace amq::ui
