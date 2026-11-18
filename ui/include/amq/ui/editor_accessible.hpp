#pragma once
// The editor's accessible-control tree (area 15 R12: every control implements the model).
//
//   Am-Q
//     Top bar        commands: Undo, Redo, EQ Match, Settings, Help
//     Response graph band nodes, by frequency (graph_accessible.hpp)
//     Band panel     the focused band's controls, from the panel model (band_panel.hpp)
//     Status bar     Processing Mode, Analyzer Freeze, Auto Gain, Gain Scale, Pan Mode, Output
//                    Level, Invert Phase, Bypass
//
// One focus (EditorFocus) is shared by the whole tree, the keys and the pointer: a band node holds
// it while the graph has a focused band and no control has taken it; a control holds it after it
// takes it, until a band node takes it back. Framework-free and unit-tested; the editor draws the
// focus ring for the control that holds it.

#include "amq/ui/band_ops.hpp"
#include "amq/ui/band_panel.hpp"
#include "amq/ui/graph_accessible.hpp"

#include <functional>
#include <map>
#include <memory>

namespace amq::ui {

// Which control, if any, holds the focus instead of the graph's focused band.
struct EditorFocus {
    const AccessibleControl* control = nullptr;
};

// A parameter: knob or value box (Slider), toggle, or choice. Actions go through gestures.
class ParamAccessible : public AccessibleControl {
public:
    ParamAccessible(EditController& c, EditorFocus& f, params::ParamIndex id, std::string name);
    std::string accessibleName() const override { return name_; }
    AccessibleRole accessibleRole() const override;
    std::string accessibleValueText() const override;
    double accessibleValueNormalised() const override;
    std::vector<AccessibleAction> accessibleActions() const override;
    // Press toggles a toggle and steps a choice; Increment/Decrement step 1/100 of a continuous
    // range or one choice; SetValue sets the normalised value.
    bool performAccessibleAction(AccessibleAction a, double value) override;
    bool accessibleEnabled() const override { return enabled_ ? enabled_() : true; }
    bool accessibleFocused() const override { return focus_.control == this; }
    bool setAccessibleFocus() override;
    [[nodiscard]] params::ParamIndex id() const noexcept { return id_; }
    std::function<bool()> enabled_;

private:
    EditController& c_;
    EditorFocus& focus_;
    params::ParamIndex id_;
    std::string name_;
};

// A command button (Undo, EQ Match, Delete band, ...).
class CommandAccessible : public AccessibleControl {
public:
    CommandAccessible(EditorFocus& f, std::string name, std::function<void()> run, std::function<bool()> enabled = {})
        : focus_(f), name_(std::move(name)), run_(std::move(run)), enabled_(std::move(enabled)) {}
    std::string accessibleName() const override { return name_; }
    AccessibleRole accessibleRole() const override { return AccessibleRole::Button; }
    std::vector<AccessibleAction> accessibleActions() const override { return {AccessibleAction::Press}; }
    bool performAccessibleAction(AccessibleAction a, double) override;
    bool accessibleEnabled() const override { return enabled_ ? enabled_() : true; }
    bool accessibleFocused() const override { return focus_.control == this; }
    bool setAccessibleFocus() override;

private:
    EditorFocus& focus_;
    std::string name_;
    std::function<void()> run_;
    std::function<bool()> enabled_;
};

// A named group whose children are computed when asked.
class GroupAccessible : public AccessibleControl {
public:
    GroupAccessible(std::string name, std::function<std::vector<AccessibleControl*>()> children)
        : name_(std::move(name)), children_(std::move(children)) {}
    std::string accessibleName() const override { return name_; }
    AccessibleRole accessibleRole() const override { return AccessibleRole::Group; }
    std::vector<AccessibleAction> accessibleActions() const override { return {}; }
    bool performAccessibleAction(AccessibleAction, double) override { return false; }
    std::vector<AccessibleControl*> accessibleChildren() const override { return children_(); }

private:
    std::string name_;
    std::function<std::vector<AccessibleControl*>()> children_;
};

// Band nodes that give the focus back to the graph when they take it.
class FocusingBandNode : public BandNodeAccessible {
public:
    FocusingBandNode(EditController& c, GraphInteraction& m, EditorFocus& f, int band)
        : BandNodeAccessible(c, m, band), focus_(f) {}
    bool accessibleFocused() const override { return focus_.control == nullptr && BandNodeAccessible::accessibleFocused(); }
    bool setAccessibleFocus() override {
        if (!BandNodeAccessible::setAccessibleFocus()) return false;
        focus_.control = nullptr;
        return true;
    }

private:
    EditorFocus& focus_;
};

// The editor's tree. Commands are bound by the editor (setCommand) before use.
class EditorAccessible {
public:
    EditorAccessible(EditController& c, GraphInteraction& m, BandOps& ops);
    [[nodiscard]] AccessibleControl& root() noexcept { return *root_; }
    [[nodiscard]] EditorFocus& focus() noexcept { return focus_; }
    // Binds a top-bar command: "Undo", "Redo", "EQ Match", "Settings", "Help".
    void setCommand(const std::string& name, std::function<void()> run, std::function<bool()> enabled = {});
    // The parameter the focused control writes, or -1 (for the focus ring).
    [[nodiscard]] long focusedParam() const noexcept;
    [[nodiscard]] std::string focusedName() const;

private:
    ParamAccessible* param(params::ParamIndex id, const std::string& name);
    std::vector<AccessibleControl*> panelChildren();
    std::vector<AccessibleControl*> graphChildren() const;

    EditController& c_;
    GraphInteraction& m_;
    BandOps& ops_;
    EditorFocus focus_;
    std::array<std::unique_ptr<FocusingBandNode>, params::kBandCount> nodes_;
    std::map<params::ParamIndex, std::unique_ptr<ParamAccessible>> params_;
    std::map<std::string, std::unique_ptr<CommandAccessible>> commands_;
    std::unique_ptr<CommandAccessible> deleteBand_;
    std::unique_ptr<GroupAccessible> top_, graph_, panel_, status_, root_;
};

}  // namespace amq::ui
