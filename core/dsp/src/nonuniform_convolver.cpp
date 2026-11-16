#include "amq/dsp/nonuniform_convolver.hpp"

#include <algorithm>

namespace amq::dsp {

std::vector<NonUniformConvolver::Segment> NonUniformConvolver::plan(int headPartition, int maxPartition, int maxKernel) {
    std::vector<Segment> p;
    if (headPartition <= 0 || maxKernel <= 0) return p;
    maxPartition = std::max(maxPartition, headPartition);
    int start = 0, part = headPartition;
    while (start < maxKernel) {
        const int len = part < maxPartition ? std::min(4 * part, maxKernel - start) : maxKernel - start;
        p.push_back({start, len, part, start + headPartition - part});
        start += len;
        part = std::min(part * 4, maxPartition);
    }
    return p;
}

bool NonUniformConvolver::prepare(int headPartition, int maxPartition, int maxKernel) {
    plan_ = plan(headPartition, maxPartition, maxKernel);
    if (plan_.empty()) return false;
    try {
        seg_.clear();
        for (size_t i = 0; i < plan_.size(); ++i) {
            seg_.push_back(std::make_unique<Seg>());
            auto& g = *seg_.back();
            g.s = plan_[i];
            if (!g.conv.prepare(g.s.partition, g.s.length, 0)) return false;
            g.line.assign(static_cast<size_t>(g.s.delay), 0.0f);
            g.pos = 0;
        }
        in_.assign(kChunk, 0.0f);
        out_.assign(kChunk, 0.0f);
    } catch (...) {
        return false;
    }
    head_ = headPartition;
    maxKernel_ = maxKernel;
    return true;
}

bool NonUniformConvolver::setKernel(const float* h, int length) {
    if (seg_.empty() || length > maxKernel_) return false;
    for (auto& gp : seg_) {
        auto& g = *gp;
        const int len = std::clamp(length - g.s.start, 0, g.s.length);
        if (!g.conv.setKernel(len > 0 ? h + g.s.start : nullptr, len)) return false;
    }
    reset();
    return true;
}

bool NonUniformConvolver::submitKernel(const float* h, int length) noexcept {
    if (seg_.empty() || length > maxKernel_ || swapInProgress()) return false;
    // All segments are free (only this writer makes them busy), so every submit succeeds.
    for (auto& gp : seg_) {
        auto& g = *gp;
        const int len = std::clamp(length - g.s.start, 0, g.s.length);
        g.conv.submitKernel(len > 0 ? h + g.s.start : nullptr, len);
    }
    return true;
}

bool NonUniformConvolver::swapInProgress() const noexcept {
    for (const auto& g : seg_)
        if (g->conv.swapInProgress()) return true;
    return false;
}

void NonUniformConvolver::process(const float* in, float* out, size_t n) noexcept {
    if (seg_.empty()) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    size_t done = 0;
    while (done < n) {
        const size_t m = std::min(kChunk, n - done);
        std::fill(out + done, out + done + m, 0.0f);
        for (auto& gp : seg_) {
            auto& g = *gp;
            const float* src = in + done;
            if (!g.line.empty()) {  // delay the segment's input by D_i
                const size_t d = g.line.size();
                for (size_t i = 0; i < m; ++i) {
                    in_[i] = g.line[g.pos];
                    g.line[g.pos] = src[i];
                    if (++g.pos == d) g.pos = 0;
                }
                src = in_.data();
            }
            g.conv.process(src, out_.data(), m);
            for (size_t i = 0; i < m; ++i) out[done + i] += out_[i];
        }
        done += m;
    }
}

void NonUniformConvolver::reset() noexcept {
    for (auto& gp : seg_) {
        auto& g = *gp;
        g.conv.reset();
        std::fill(g.line.begin(), g.line.end(), 0.0f);
        g.pos = 0;
    }
}

}  // namespace amq::dsp
