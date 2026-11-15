#pragma once
// GUI gesture helper (area 09 R4; area 13 R18): one begin per parameter when the gesture opens,
// any number of performs, and one end per parameter when it closes, also on early destruction.
// A multi-parameter drag (a graph node moves frequency and gain) opens one gesture per
// parameter and closes them together. A gesture is one undo step (undo arrives with area 12).

#include "amq/ui/edit_controller.hpp"

#include <initializer_list>
#include <vector>

namespace amq::ui {

class EditGesture {
public:
    EditGesture() = default;
    EditGesture(EditController& c, std::initializer_list<params::ParamIndex> ids) { open(c, ids); }
    EditGesture(const EditGesture&) = delete;
    EditGesture& operator=(const EditGesture&) = delete;
    EditGesture(EditGesture&& o) noexcept : c_(o.c_), ids_(std::move(o.ids_)) { o.c_ = nullptr; }
    EditGesture& operator=(EditGesture&& o) noexcept {
        if (this != &o) {
            close();
            c_ = o.c_;
            ids_ = std::move(o.ids_);
            o.c_ = nullptr;
        }
        return *this;
    }
    ~EditGesture() { close(); }

    template <typename Ids>
    void open(EditController& c, const Ids& ids) {
        close();
        c_ = &c;
        for (auto id : ids) {
            if (!params::specAt(id)) continue;
            ids_.push_back(id);
            c.beginEdit(id);
        }
    }
    void open(EditController& c, std::initializer_list<params::ParamIndex> ids) { open<std::initializer_list<params::ParamIndex>>(c, ids); }

    [[nodiscard]] bool isOpen() const noexcept { return c_ != nullptr; }

    // Writes a value of one of the gesture's parameters (clamped by the table).
    void set(params::ParamIndex id, double plain) {
        if (c_) c_->performEdit(id, plain);
    }

    void close() {
        if (!c_) return;
        for (auto id : ids_) c_->endEdit(id);
        ids_.clear();
        c_->dispatchNotifications();
        c_ = nullptr;
    }

private:
    EditController* c_ = nullptr;
    std::vector<params::ParamIndex> ids_;
};

}  // namespace amq::ui
