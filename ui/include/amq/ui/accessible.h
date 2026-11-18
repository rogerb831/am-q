#pragma once
// Accessible-control model (area 15 R12, docs/plan/decisions.md D1).
//
// Every interactive control and band node implements AccessibleControl from its first
// version. In v1 the model drives keyboard focus order and navigation (area 15 R3), the focus
// indicator and scripted GUI tests, which find controls by name and role. After v1 a
// platform bridge maps the same tree to UIA (Windows) and NSAccessibility (macOS) without
// changing the controls.
//
// Threading: main thread only, like the rest of the editor.

#include <cstdint>
#include <string>
#include <vector>

namespace amq::ui {

enum class AccessibleRole : uint8_t {
    Group,      // a region: graph, band panel, header, footer
    Button,
    Toggle,
    Slider,     // continuous value
    Choice,     // menu or stepped list
    TextField,
    GraphNode,  // a band node on the response graph
};

enum class AccessibleAction : uint8_t {
    Press,
    Increment,  // one step up (arrow nudge)
    Decrement,
    SetValue,   // set the normalised value
    OpenMenu,
};

class AccessibleControl {
public:
    virtual ~AccessibleControl() = default;

    // Stable, user-facing name, e.g. "Band 3 Frequency". Never empty for interactive controls.
    virtual std::string accessibleName() const = 0;
    virtual AccessibleRole accessibleRole() const = 0;

    // Current value as display text, from the parameter formatters (area 10), and as a
    // normalised 0..1 number. Controls without a value return "" and a negative number.
    virtual std::string accessibleValueText() const { return {}; }
    virtual double accessibleValueNormalised() const { return -1.0; }

    virtual std::vector<AccessibleAction> accessibleActions() const = 0;
    // Performs an action through the standard setter path (set(id, plain, GUI)) with a gesture.
    // Returns false if the action is not supported or the control is disabled.
    virtual bool performAccessibleAction(AccessibleAction action, double value = 0.0) = 0;

    virtual bool accessibleEnabled() const { return true; }
    virtual bool accessibleVisible() const { return true; }

    // Children in focus order (for groups). Leaf controls return an empty list.
    virtual std::vector<AccessibleControl*> accessibleChildren() const { return {}; }

    // Focus (area 15 R3, R12). One focus is shared by the keys and assistive tools: a control
    // reports whether it holds the editor's focus and can be asked to take it. Groups are not
    // focusable themselves; their focusable descendants are.
    virtual bool accessibleFocusable() const {
        return accessibleRole() != AccessibleRole::Group && accessibleEnabled() && accessibleVisible();
    }
    virtual bool accessibleFocused() const { return false; }
    // Takes the editor's focus; returns false if the control cannot.
    virtual bool setAccessibleFocus() { return false; }
};

// The focusable controls under root, in focus order (depth first, children in their order).
inline void accessibleFocusOrder(AccessibleControl& root, std::vector<AccessibleControl*>& out) {
    if (root.accessibleFocusable()) out.push_back(&root);
    for (AccessibleControl* c : root.accessibleChildren())
        if (c && c->accessibleVisible()) accessibleFocusOrder(*c, out);
}

// The control holding the focus, or nullptr.
inline AccessibleControl* accessibleFocusedControl(AccessibleControl& root) {
    std::vector<AccessibleControl*> order;
    accessibleFocusOrder(root, order);
    for (AccessibleControl* c : order)
        if (c->accessibleFocused()) return c;
    return nullptr;
}

// Moves the focus to the next (direction > 0) or previous focusable control, wrapping; with no
// focus, to the first or last. Returns the newly focused control, or nullptr if there is none.
inline AccessibleControl* accessibleFocusNext(AccessibleControl& root, int direction) {
    std::vector<AccessibleControl*> order;
    accessibleFocusOrder(root, order);
    if (order.empty()) return nullptr;
    const int n = static_cast<int>(order.size());
    int k = -1;
    for (int i = 0; i < n; ++i)
        if (order[static_cast<size_t>(i)]->accessibleFocused()) k = i;
    k = k < 0 ? (direction > 0 ? 0 : n - 1) : ((k + (direction > 0 ? 1 : -1)) % n + n) % n;
    AccessibleControl* c = order[static_cast<size_t>(k)];
    return c->setAccessibleFocus() ? c : nullptr;
}

}  // namespace amq::ui
