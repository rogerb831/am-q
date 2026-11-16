#include "amq/ui/undo_controller.hpp"

#include <algorithm>

namespace amq::ui {

void UndoController::openStep(const std::string& label) {
    if (recording_) return;
    current_ = Step{label, {}};
    recording_ = true;
}

void UndoController::closeStepIfDone() {
    if (!recording_ || gestures_ > 0 || groups_ > 0) return;
    recording_ = false;
    // Drop parameters that ended where they started; drop the step if nothing changed.
    auto& c = current_.changes;
    c.erase(std::remove_if(c.begin(), c.end(), [](const Change& x) { return x.before == x.after; }), c.end());
    if (c.empty()) return;
    undo_.push_back(std::move(current_));
    while (undo_.size() > depth_) undo_.pop_front();
    redo_.clear();
}

void UndoController::beginEdit(params::ParamIndex index) {
    if (!replaying_) {
        openStep("Edit");
        ++gestures_;
    }
    in_.beginEdit(index);
}

void UndoController::performEdit(params::ParamIndex index, double plain) {
    if (!replaying_ && recording_) {
        auto it = std::find_if(current_.changes.begin(), current_.changes.end(),
                               [&](const Change& c) { return c.index == index; });
        if (it == current_.changes.end()) {
            current_.changes.push_back({index, in_.table().value(index), 0.0});
            it = current_.changes.end() - 1;
        }
        in_.performEdit(index, plain);
        it->after = in_.table().value(index);  // as stored: clamped by the table
        return;
    }
    in_.performEdit(index, plain);
}

void UndoController::endEdit(params::ParamIndex index) {
    in_.endEdit(index);
    if (replaying_) return;
    if (gestures_ > 0) --gestures_;
    closeStepIfDone();
}

void UndoController::beginGroup(std::string label) {
    if (groups_ == 0 && !recording_) openStep(label);
    else if (groups_ == 0) current_.label = std::move(label);
    ++groups_;
}

bool UndoController::reopenLast(const std::string& label) {
    if (recording_ || replaying_ || undo_.empty() || undo_.back().label != label) return false;
    current_ = std::move(undo_.back());
    undo_.pop_back();
    redo_.clear();
    recording_ = true;
    ++groups_;
    return true;
}

void UndoController::endGroup() {
    if (groups_ > 0) --groups_;
    closeStepIfDone();
}

void UndoController::apply(const Step& s, bool forward) {
    replaying_ = true;
    for (const auto& c : s.changes) in_.beginEdit(c.index);
    // Undo restores in reverse order, redo replays in order.
    if (forward)
        for (const auto& c : s.changes) in_.performEdit(c.index, c.after);
    else
        for (auto it = s.changes.rbegin(); it != s.changes.rend(); ++it) in_.performEdit(it->index, it->before);
    for (const auto& c : s.changes) in_.endEdit(c.index);
    replaying_ = false;
    in_.dispatchNotifications();
}

bool UndoController::undo() {
    if (undo_.empty() || recording_) return false;
    Step s = std::move(undo_.back());
    undo_.pop_back();
    apply(s, false);
    redo_.push_back(std::move(s));
    return true;
}

bool UndoController::redo() {
    if (redo_.empty() || recording_) return false;
    Step s = std::move(redo_.back());
    redo_.pop_back();
    apply(s, true);
    undo_.push_back(std::move(s));
    return true;
}

void UndoController::setDepth(size_t steps) {
    depth_ = std::max<size_t>(steps, 1);
    while (undo_.size() > depth_) undo_.pop_front();
    while (redo_.size() > depth_) redo_.pop_front();
}

void UndoController::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace amq::ui
