#pragma once
// GUI parameter attachment (area 10 R6; area 13 R22): binds a control to one parameter by its
// stable string id, never by index. It reads the value, formats it, edits it through gestures,
// and calls onChange when the value changes from anywhere (host, preset, another control).
// Visual style is separate and carries no binding data.

#include "amq/ui/edit_gesture.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace amq::ui {

class ParamAttachment : private params::ParameterListener {
public:
    ParamAttachment(EditController& c, std::string_view id) : c_(c) {
        const auto idx = params::findById(id);
        if (!idx) return;
        index_ = *idx;
        valid_ = true;
        c_.addListener(this);
    }
    ~ParamAttachment() override {
        if (valid_) c_.removeListener(this);
    }
    ParamAttachment(const ParamAttachment&) = delete;
    ParamAttachment& operator=(const ParamAttachment&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] params::ParamIndex index() const noexcept { return index_; }
    [[nodiscard]] const params::ParamSpec& spec() const noexcept { return *params::specAt(index_); }
    [[nodiscard]] double value() const noexcept { return valid_ ? c_.table().value(index_) : 0.0; }
    [[nodiscard]] double normalised() const noexcept { return valid_ ? params::toNormalised(spec(), value()) : 0.0; }
    [[nodiscard]] std::string text() const { return valid_ ? params::formatPlain(spec(), value()) : std::string(); }

    void beginGesture() {
        if (valid_) gesture_.open(c_, {index_});
    }
    void setDuringGesture(double plain) { gesture_.set(index_, plain); }
    void endGesture() { gesture_.close(); }

    // One complete edit (typed entry, double-click reset, menu pick): begin, perform, end.
    void setValue(double plain) {
        if (!valid_) return;
        EditGesture g(c_, {index_});
        g.set(index_, plain);
    }
    void setNormalised(double n) {
        if (valid_) setValue(params::fromNormalised(spec(), n));
    }
    bool setText(std::string_view text) {
        if (!valid_) return false;
        const auto v = params::parsePlain(spec(), text);
        if (!v) return false;
        setValue(*v);
        return true;
    }

    std::function<void(double plain, params::ParamSource source)> onChange;

private:
    void parameterChanged(params::ParamIndex i, double plain, params::ParamSource src) override {
        if (i == index_ && onChange) onChange(plain, src);
    }

    EditController& c_;
    params::ParamIndex index_ = 0;
    bool valid_ = false;
    EditGesture gesture_;
};

}  // namespace amq::ui
