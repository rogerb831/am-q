// The lane-set step's dispatch (dynamics_lanes.hpp). Kept out of dynamics_avx2.cpp: that file is
// compiled with AVX2, and the scalar kernel called here must run on any x86-64 CPU.

#include "amq/dsp/dynamics_lanes.hpp"

namespace amq::dsp {

void gainLanesStep(GainLaneSet& s, const double* env, size_t count) noexcept {
    static const bool avx2 = cpuHasAvx2();
    size_t i = avx2 ? gainLanesStepAvx2(s, env, count) : 0;
    for (; i < count; ++i) {  // the rest one by one, through the scalar kernel itself
        GainLane L = s.lane(i);
        double x = env[i];
        gainLaneScalar(L, &x, 1);
        s.refCount[i] = L.refCount, s.ref[i] = L.ref, s.fy[i] = L.fy, s.sy[i] = L.sy, s.zy[i] = L.zy;
    }
}

}  // namespace amq::dsp
