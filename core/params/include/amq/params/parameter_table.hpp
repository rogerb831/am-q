#pragma once
// Am-Q parameter table: the single write path for every parameter (area 09 R1, area 10 R1).
//
// Threading:
//   - set() and value() are lock-free and safe on any thread. The plugin calls set() on the
//     audio thread for host automation, and on the main thread for GUI, preset, EQ Match and
//     undo writes. Each value is one atomic slot, read by the DSP once per block.
//   - Listeners are never called from set(). set() marks the parameter dirty;
//     dispatchNotifications() (main thread only) delivers one coalesced call per changed
//     parameter with the latest value and source.
//
// No allocation after construction.

#include "amq/params/param_spec.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace amq::params {

enum class SetResult : uint8_t { Changed, Unchanged, BadIndex };

class ParameterListener {
public:
    virtual ~ParameterListener() = default;
    // Main thread, from dispatchNotifications(). plain is the latest stored value.
    virtual void parameterChanged(ParamIndex index, double plain, ParamSource source) = 0;
};

class ParameterTable {
public:
    ParameterTable();
    ParameterTable(const ParameterTable&) = delete;
    ParameterTable& operator=(const ParameterTable&) = delete;

    [[nodiscard]] static constexpr ParamIndex size() noexcept { return kParamCount; }

    // Clamp, compare (sub-step changes are ignored), store, mark dirty. Any thread.
    SetResult set(ParamIndex index, double plain, ParamSource source) noexcept;
    SetResult setNormalised(ParamIndex index, double normalised, ParamSource source) noexcept;
    SetResult setFromHost(ParamIndex index, double hostValue, ParamSource source) noexcept;

    // Bounds-checked read. Any thread.
    [[nodiscard]] std::optional<double> get(ParamIndex index) const noexcept;
    // Unchecked fast read for the audio thread; index must be < size().
    [[nodiscard]] double value(ParamIndex index) const noexcept {
        return values_[index].load(std::memory_order_relaxed);
    }

    // Every parameter back to its default, as one batch with the given source (state reset).
    void resetToDefaults(ParamSource source) noexcept;

    // Main thread only.
    void addListener(ParameterListener* l);
    void removeListener(ParameterListener* l);
    // Delivers pending changes; returns how many parameters were reported.
    int dispatchNotifications();
    [[nodiscard]] bool hasPendingNotifications() const noexcept;

private:
    static constexpr size_t kWords = (kParamCount + 63) / 64;
    std::unique_ptr<std::atomic<double>[]> values_;
    std::unique_ptr<std::atomic<uint8_t>[]> sources_;
    std::unique_ptr<std::atomic<uint64_t>[]> dirty_;
    std::vector<ParameterListener*> listeners_;
};

// The display context for text that depends on other parameters (area 04 R6). Any thread.
[[nodiscard]] DisplayContext displayContext(const ParameterTable& t) noexcept;
// Current display text of one parameter, with its context. Any thread.
[[nodiscard]] std::string displayText(const ParameterTable& t, ParamIndex index);

// True if a and b differ by less than one representable step of the parameter: exact index
// compare for toggles and choices, 1e-9 of the range for continuous values (area 09 R1).
[[nodiscard]] bool sameValue(const ParamSpec& s, double a, double b) noexcept;

}  // namespace amq::params
