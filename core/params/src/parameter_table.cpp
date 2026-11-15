#include "amq/params/parameter_table.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace amq::params {

bool sameValue(const ParamSpec& s, double a, double b) noexcept {
    if (s.type != ParamType::Continuous) return a == b;
    return std::abs(a - b) <= 1e-9 * (s.max - s.min);
}

ParameterTable::ParameterTable()
    : values_(new std::atomic<double>[kParamCount]),
      sources_(new std::atomic<uint8_t>[kParamCount]),
      dirty_(new std::atomic<uint64_t>[kWords]) {
    const auto& specs = parameterSpecs();
    for (ParamIndex i = 0; i < kParamCount; ++i) {
        values_[i].store(specs[i].def, std::memory_order_relaxed);
        sources_[i].store(static_cast<uint8_t>(ParamSource::Preset), std::memory_order_relaxed);
    }
    for (size_t w = 0; w < kWords; ++w) dirty_[w].store(0, std::memory_order_relaxed);
}

SetResult ParameterTable::set(ParamIndex index, double plain, ParamSource source) noexcept {
    const ParamSpec* s = specAt(index);
    if (!s) return SetResult::BadIndex;
    const double v = clampPlain(*s, plain);
    const double old = values_[index].load(std::memory_order_relaxed);
    if (sameValue(*s, old, v)) return SetResult::Unchanged;
    values_[index].store(v, std::memory_order_relaxed);
    sources_[index].store(static_cast<uint8_t>(source), std::memory_order_relaxed);
    dirty_[index / 64].fetch_or(uint64_t{1} << (index % 64), std::memory_order_release);
    return SetResult::Changed;
}

SetResult ParameterTable::setNormalised(ParamIndex index, double normalised, ParamSource source) noexcept {
    const ParamSpec* s = specAt(index);
    if (!s) return SetResult::BadIndex;
    return set(index, fromNormalised(*s, normalised), source);
}

SetResult ParameterTable::setFromHost(ParamIndex index, double hostValue, ParamSource source) noexcept {
    const ParamSpec* s = specAt(index);
    if (!s) return SetResult::BadIndex;
    return set(index, fromHost(*s, hostValue), source);
}

std::optional<double> ParameterTable::get(ParamIndex index) const noexcept {
    if (index >= kParamCount) return std::nullopt;
    return value(index);
}

void ParameterTable::resetToDefaults(ParamSource source) noexcept {
    const auto& specs = parameterSpecs();
    for (ParamIndex i = 0; i < kParamCount; ++i) set(i, specs[i].def, source);
}

void ParameterTable::addListener(ParameterListener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) listeners_.push_back(l);
}

void ParameterTable::removeListener(ParameterListener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

bool ParameterTable::hasPendingNotifications() const noexcept {
    for (size_t w = 0; w < kWords; ++w)
        if (dirty_[w].load(std::memory_order_acquire)) return true;
    return false;
}

int ParameterTable::dispatchNotifications() {
    int count = 0;
    for (size_t w = 0; w < kWords; ++w) {
        uint64_t bits = dirty_[w].exchange(0, std::memory_order_acquire);
        while (bits) {
            const int b = std::countr_zero(bits);
            bits &= bits - 1;
            const auto index = static_cast<ParamIndex>(w * 64 + static_cast<size_t>(b));
            const double v = values_[index].load(std::memory_order_relaxed);
            const auto src = static_cast<ParamSource>(sources_[index].load(std::memory_order_relaxed));
            for (auto* l : listeners_) l->parameterChanged(index, v, src);
            ++count;
        }
    }
    return count;
}

}  // namespace amq::params
