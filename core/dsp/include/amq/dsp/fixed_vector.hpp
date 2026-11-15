#pragma once
// Fixed-capacity vector: the std::vector subset core/dsp uses, with inline storage and no heap
// allocation, so the analog prototype and the three-point / bilinear designs can run on the
// audio thread (area 01 R19).
//
// Capacity is sized from the design limits (order <= 16, see analog_prototype.hpp). Pushing
// past capacity drops the element and sets overflowed(); tests sweep every shape and slope to
// prove it never happens.

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace amq::dsp {

template <typename T, std::size_t N>
class FixedVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    FixedVector() = default;
    FixedVector(size_type n, const T& v) { assign(n, v); }
    FixedVector(std::initializer_list<T> init) {
        for (const auto& v : init) push_back(v);
    }

    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

    T* data() noexcept { return items_; }
    const T* data() const noexcept { return items_; }
    iterator begin() noexcept { return items_; }
    iterator end() noexcept { return items_ + size_; }
    const_iterator begin() const noexcept { return items_; }
    const_iterator end() const noexcept { return items_ + size_; }

    T& operator[](size_type i) noexcept { return items_[i]; }
    const T& operator[](size_type i) const noexcept { return items_[i]; }
    T& front() noexcept { return items_[0]; }
    const T& front() const noexcept { return items_[0]; }
    T& back() noexcept { return items_[size_ - 1]; }
    const T& back() const noexcept { return items_[size_ - 1]; }

    void push_back(const T& v) noexcept {
        if (size_ < N) items_[size_++] = v;
        else overflowed_ = true;
    }
    template <typename... A>
    T& emplace_back(A&&... args) noexcept {
        push_back(T(std::forward<A>(args)...));
        return back();
    }
    void pop_back() noexcept {
        if (size_) --size_;
    }
    void clear() noexcept { size_ = 0; }
    void reserve(size_type n) noexcept {
        if (n > N) overflowed_ = true;
    }
    void resize(size_type n) noexcept { resize(n, T{}); }
    void resize(size_type n, const T& v) noexcept {
        if (n > N) {
            overflowed_ = true;
            n = N;
        }
        for (size_type i = size_; i < n; ++i) items_[i] = v;
        size_ = n;
    }
    void assign(size_type n, const T& v) noexcept {
        size_ = 0;
        resize(n, v);
    }

private:
    T items_[N]{};
    size_type size_ = 0;
    bool overflowed_ = false;
};

}  // namespace amq::dsp
