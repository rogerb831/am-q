#pragma once
// The spectral stage's apply step (area 07): each bin's gain change in dB is the band's relative
// response scaled by the reduction interpolated from the two analysis bands that weight the bin.
//
//   frac = (w[k] g[a[k]] + (1 - w[k]) g[min(a[k] + 1, last)]) inv
//   if !(frac <= 0):  comps[j][k] += min(frac, 1) rel[k]     for every component j
//
// spectralApplyAvx2 does bins [k0, k0 + 4m) four at a time with the scalar form's operations in the
// same order and no fused multiply-adds, so each bin gets the same bits; the caller finishes the
// rest. Call it only when cpuHasAvx2() is true (dynamics_lanes.hpp).

#include <cstdint>

namespace amq::dsp {

struct SpectralApplyArgs {
    const int* band = nullptr;       // per bin: the lower analysis band
    const float* weight = nullptr;   // per bin: its weight
    const float* relDb = nullptr;    // per bin: the band's relative response in dB
    const double* grDb = nullptr;    // per analysis band: the reduction in dB
    int last = 0;                    // the last analysis band
    double inv = 0.0;                // 1 / the band's range
    double* comps[4] = {};           // the components to add to
    int nc = 0;
};

// Returns the first bin it did not do.
int spectralApplyAvx2(const SpectralApplyArgs& a, int k0, int k1) noexcept;

}  // namespace amq::dsp
