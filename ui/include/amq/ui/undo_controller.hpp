#pragma once
// Bounded undo history (area 12 task "Add bounded undo history with a depth setting"; area 13 R19;
// area 14 R4; area 21 R5, R8, R14).
//
// UndoController wraps the plugin's EditController and is what the editor and the band-operations
// service talk to. It records GUI edits only (host automation and state loads never reach it, so a
// load adds no undo entries, area 21 R14):
//   - one gesture (begin ... end, possibly over several parameters) is one undo step;
//   - an explicit group (UndoGroup, used by every band operation) makes all gestures inside it one
//     step (area 14 R4; band creation with its defaults, area 21 R8);
//   - each step keeps, per parameter, the value before its first change and the value after its
//     last one; steps that change nothing are dropped.
// undo() and redo() write the stored values back through the wrapped controller as one gesture, so
// the host records them like any edit. A new step clears the redo stack. The depth (default 100,
// PROVISIONAL) bounds the history; the oldest steps fall off. clear() empties both stacks (the
// plugin calls it after a preset or state load). Main (GUI) thread only.

#include "amq/ui/edit_controller.hpp"

#include <deque>
#include <string>
#include <vector>

namespace amq::ui {

class UndoController : public EditController {
public:
    static constexpr size_t kDefaultDepth = 100;

    explicit UndoController(EditController& inner) : in_(inner) {}

    // EditController: forwarded, with recording.
    [[nodiscard]] const params::ParameterTable& table() const noexcept override { return in_.table(); }
    void addListener(params::ParameterListener* l) override { in_.addListener(l); }
    void removeListener(params::ParameterListener* l) override { in_.removeListener(l); }
    void dispatchNotifications() override { in_.dispatchNotifications(); }
    void beginEdit(params::ParamIndex index) override;
    void performEdit(params::ParamIndex index, double plain) override;
    void endEdit(params::ParamIndex index) override;
    const analysis::LevelSnapshot* pullLevels() noexcept override { return in_.pullLevels(); }
    const analysis::GainChangeSnapshot* pullGainChange() noexcept override { return in_.pullGainChange(); }
    const analysis::SpectrumSnapshot* pullSpectrum() noexcept override { return in_.pullSpectrum(); }

    // Explicit grouping (nests; the outermost end commits). The label names the step in menus.
    void beginGroup(std::string label);
    void endGroup();

    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    [[nodiscard]] std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    [[nodiscard]] size_t undoSteps() const noexcept { return undo_.size(); }

    void setDepth(size_t steps);
    [[nodiscard]] size_t depth() const noexcept { return depth_; }
    void clear();

private:
    struct Change {
        params::ParamIndex index = 0;
        double before = 0.0, after = 0.0;
    };
    struct Step {
        std::string label;
        std::vector<Change> changes;
    };
    void openStep(const std::string& label);
    void closeStepIfDone();
    void apply(const Step& s, bool forward);

    EditController& in_;
    std::deque<Step> undo_, redo_;
    Step current_;
    bool recording_ = false;  // a step is open
    int gestures_ = 0;        // open gesture parameters
    int groups_ = 0;          // open explicit groups
    bool replaying_ = false;  // undo / redo writing: not recorded
    size_t depth_ = kDefaultDepth;
};

// RAII group: every edit inside is one undo step.
class UndoGroup {
public:
    UndoGroup(UndoController& c, std::string label) : c_(c) { c_.beginGroup(std::move(label)); }
    ~UndoGroup() { c_.endGroup(); }
    UndoGroup(const UndoGroup&) = delete;
    UndoGroup& operator=(const UndoGroup&) = delete;

private:
    UndoController& c_;
};

}  // namespace amq::ui
