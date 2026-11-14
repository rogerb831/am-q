#include "amq/dsp/digital_design.hpp"

#include "amq/dsp/conventions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace amq::dsp {
namespace {

using cd = std::complex<double>;

// exp(z) - 1 without cancellation for small |z|.
cd complexExpm1(cd z) {
    const double em1 = std::expm1(z.real());
    const double h = std::sin(0.5 * z.imag());
    const double cosm1 = -2.0 * h * h;
    return {em1 * std::cos(z.imag()) + cosm1, (em1 + 1.0) * std::sin(z.imag())};
}

// Roots of the section denominator a0 + a1 s + a2 s^2 (normalised s).
int denominatorRoots(const AnalogSection& s, cd roots[2]) {
    if (s.a2 != 0.0) {
        const cd disc = std::sqrt(cd{s.a1 * s.a1 - 4.0 * s.a2 * s.a0, 0.0});
        // Numerically stable quadratic formula.
        const cd qv = -0.5 * (s.a1 + (s.a1 >= 0.0 ? disc : -disc));
        roots[0] = qv / s.a2;
        roots[1] = (std::abs(qv) > 0.0) ? s.a0 / qv : cd{0.0, 0.0};
        return 2;
    }
    if (s.a1 != 0.0) {
        roots[0] = -s.a0 / s.a1;
        return 1;
    }
    return 0;
}

// The phi basis of Vicanek (2016): for omega in [0, pi]
//   phi1 = sin^2(omega/2), phi0 = 1 - phi1, phi2 = 4 phi0 phi1,
// and |b0 + b1 z^-1 + b2 z^-2|^2 = B0 phi0 + B1 phi1 + B2 phi2 with
//   B0 = (b0+b1+b2)^2, B1 = (b0-b1+b2)^2, B2 = -4 b0 b2.
struct Phi {
    double p0, p1, p2;
};
Phi phiAt(double omega) {
    const double sh = std::sin(0.5 * omega);
    const double p1 = sh * sh;
    const double ch = std::cos(0.5 * omega);
    const double p0 = ch * ch;
    return {p0, p1, 4.0 * p0 * p1};
}

// Analog section magnitude squared at digital frequency omega.
double analogMag2(const AnalogSection& s, double omega, double omegaC) {
    return std::norm(s.evaluate(omega / omegaC));
}

// Digital denominator magnitude squared prod |1 - z_i e^{-j omega}|^2, z_i = exp(p_i),
// evaluated with expm1 so it stays accurate when poles sit near z = 1.
double denominatorMag2(const cd* polesT, int count, double omega) {
    double d = 1.0;
    for (int i = 0; i < count; ++i) d *= std::norm(complexExpm1(polesT[i] - cd{0.0, omega}));
    return d;
}

Biquad matchSection(const AnalogSection& s, double omegaC, bool& clamped) {
    Biquad bq;
    cd roots[2];
    const int nPoles = denominatorRoots(s, roots);
    cd polesT[2];
    for (int i = 0; i < nPoles; ++i) polesT[i] = roots[i] * omegaC;  // poles in rad/sample

    if (nPoles == 0) {  // pure gain
        bq.b0 = s.b0 / s.a0;
        return bq;
    }

    // Exact pole mapping z = exp(p T) (impulse-invariant poles).
    if (nPoles == 2) {
        const cd z0 = std::exp(polesT[0]);
        const cd z1 = std::exp(polesT[1]);
        bq.a1 = -(z0 + z1).real();
        bq.a2 = (z0 * z1).real();
    } else {
        bq.a1 = -std::exp(polesT[0]).real();
        bq.a2 = 0.0;
    }

    // Targets at DC and Nyquist.
    const double B0 = analogMag2(s, 0.0, omegaC) * denominatorMag2(polesT, nPoles, 0.0);
    const double B1 = analogMag2(s, kPi, omegaC) * denominatorMag2(polesT, nPoles, kPi);
    const double W = std::sqrt(std::max(B0, 0.0));
    const double V = std::sqrt(std::max(B1, 0.0));

    if (nPoles == 1) {
        // First-order section: b0 + b1 z^-1, matched at DC and Nyquist, minimum phase.
        bq.b0 = 0.5 * (W + V);
        bq.b1 = 0.5 * (W - V);
        bq.b2 = 0.0;
        return bq;
    }

    // Third match point: the section's natural frequency, kept inside (0, 0.9 pi].
    const double naturalOmega = std::sqrt(std::abs(s.a0 / s.a2)) * omegaC;
    const double omegaM = std::clamp(naturalOmega, 1e-9, 0.9 * kPi);
    const Phi ph = phiAt(omegaM);
    const double target = analogMag2(s, omegaM, omegaC) * denominatorMag2(polesT, nPoles, omegaM);
    double B2 = (target - B0 * ph.p0 - B1 * ph.p1) / ph.p2;

    // Recover b0, b1, b2. With s0 = b0+b1+b2 = W and s1 = b0-b1+b2 = +/-V:
    //   b0 + b2 = (s0 + s1)/2 = P, b1 = (s0 - s1)/2, b0*b2 = -B2/4,
    // so b0, b2 are the roots of x^2 - P x - B2/4 = 0 (real iff P^2 + B2 >= 0).
    // Both sign choices give the same magnitude; take the first realisable one.
    auto trySolve = [&](double s1, Biquad& out) {
        const double P = 0.5 * (W + s1);
        const double disc = P * P + B2;
        if (disc < 0.0) return false;
        const double root = std::sqrt(disc);
        const double big = 0.5 * (P + (P >= 0.0 ? root : -root));
        out.b0 = big;
        out.b2 = big != 0.0 ? (-0.25 * B2) / big : P - big;
        out.b1 = 0.5 * (W - s1);
        return true;
    };
    if (trySolve(V, bq) || trySolve(-V, bq)) return bq;

    // Unrealisable target: clamp B2 to the realisable limit (double zero on the unit
    // circle). The match at omegaM is then not exact.
    clamped = true;
    const double P = 0.5 * (W + V);
    B2 = -P * P;
    bq.b0 = 0.5 * P;
    bq.b2 = 0.5 * P;
    bq.b1 = 0.5 * (W - V);
    return bq;
}

// Maps an analog pole (rad/sample units) to z. Exact exp() mapping unless the pole's
// frequency is near or above Nyquist, where exp() would alias it to a wrong frequency.
// OPEN QUESTION (Phase 0): the best treatment of poles above Nyquist. The plan's risk
// table suggests an oversampled design for extreme settings.
constexpr double kPoleAliasLimit = 0.95 * kPi;
cd mapPole(cd pT) {
    if (std::abs(pT.imag()) <= kPoleAliasLimit) return std::exp(pT);
    // Keep the decay (real part), pull the frequency just below Nyquist; the numerator
    // fit then compensates the remaining shape error below Nyquist.
    return std::exp(cd{pT.real(), std::copysign(kPoleAliasLimit, pT.imag())});
}

// Recovers b0, b1, b2 from B0, B1, B2 (same algebra as the three-point match).
// Returns false if the target is not realisable.
bool numeratorFromB(double B0, double B1, double B2, Biquad& bq) {
    const double W = std::sqrt(std::max(B0, 0.0));
    const double V = std::sqrt(std::max(B1, 0.0));
    for (double s1 : {V, -V}) {
        const double P = 0.5 * (W + s1);
        const double disc = P * P + B2;
        if (disc < 0.0) continue;
        const double root = std::sqrt(disc);
        const double big = 0.5 * (P + (P >= 0.0 ? root : -root));
        bq.b0 = big;
        bq.b2 = big != 0.0 ? (-0.25 * B2) / big : P - big;
        bq.b1 = 0.5 * (W - s1);
        return true;
    }
    return false;
}

// Solves the small weighted least-squares problem min ||A x - y|| (n x m, m <= 3)
// by modified Gram-Schmidt QR. Returns false if rank-deficient.
bool leastSquares(std::vector<std::array<double, 3>>& A, std::vector<double>& y, int m, double x[3]) {
    const size_t n = y.size();
    double R[3][3] = {};
    double qy[3] = {};
    for (int j = 0; j < m; ++j) {
        for (int k = 0; k < j; ++k) {
            double dot = 0.0;
            for (size_t i = 0; i < n; ++i) dot += A[i][k] * A[i][j];
            R[k][j] = dot;
            for (size_t i = 0; i < n; ++i) A[i][j] -= dot * A[i][k];
        }
        double norm = 0.0;
        for (size_t i = 0; i < n; ++i) norm += A[i][j] * A[i][j];
        norm = std::sqrt(norm);
        if (!(norm > 0.0)) return false;
        R[j][j] = norm;
        for (size_t i = 0; i < n; ++i) A[i][j] /= norm;
    }
    for (int j = 0; j < m; ++j) {
        double dot = 0.0;
        for (size_t i = 0; i < n; ++i) dot += A[i][j] * y[i];
        qy[j] = dot;
    }
    for (int j = m - 1; j >= 0; --j) {
        double v = qy[j];
        for (int k = j + 1; k < m; ++k) v -= R[j][k] * x[k];
        x[j] = v / R[j][j];
    }
    return true;
}

Biquad fitSection(const AnalogSection& s, double omegaC, bool& clamped) {
    Biquad bq;
    cd roots[2];
    const int nPoles = denominatorRoots(s, roots);
    if (nPoles == 0) {
        bq.b0 = s.b0 / s.a0;
        return bq;
    }
    cd polesT[2];
    cd zs[2];
    for (int i = 0; i < nPoles; ++i) {
        polesT[i] = roots[i] * omegaC;
        zs[i] = mapPole(polesT[i]);
    }
    if (nPoles == 2) {
        bq.a1 = -(zs[0] + zs[1]).real();
        bq.a2 = (zs[0] * zs[1]).real();
    } else {
        bq.a1 = -zs[0].real();
        bq.a2 = 0.0;
    }
    // Digital denominator |A|^2 from the mapped poles, written with expm1 where the
    // exact map was used (accurate near z = 1).
    auto denMag2 = [&](double omega) {
        double d = 1.0;
        for (int i = 0; i < nPoles; ++i) {
            if (std::abs(polesT[i].imag()) <= kPoleAliasLimit) d *= std::norm(complexExpm1(polesT[i] - cd{0.0, omega}));
            else d *= std::norm(1.0 - zs[i] * std::polar(1.0, -omega));
        }
        return d;
    };

    // Fit grid: log-spaced from ~1e-4 rad/sample (about 1 Hz at 48 kHz) to Nyquist.
    constexpr int kGrid = 160;
    std::vector<double> omegas;
    omegas.reserve(kGrid + 1);
    const double wLo = 1e-4;
    for (int i = 0; i < kGrid; ++i) omegas.push_back(wLo * std::pow(kPi / wLo, i / double(kGrid - 1)));
    std::vector<double> target(omegas.size());
    for (size_t k = 0; k < omegas.size(); ++k) target[k] = analogMag2(s, omegas[k], omegaC) * denMag2(omegas[k]);

    // First-order section: numerator b0 + b1 z^-1 has |.|^2 = B0 phi0 + B1 phi1.
    // Second-order: + B2 phi2. A numerator zero at s = 0 forces B0 = 0.
    const bool zeroAtDc = s.b0 == 0.0;
    const bool firstOrder = nPoles == 1 && s.b2 == 0.0;

    // Only fit where the section is within kFitRangeDb of its own peak: far below that
    // the band is already below the comparison floor, and relative errors there would
    // otherwise dominate the fit.
    constexpr double kFitRangePower = 1e-8;  // -80 dB
    double tMax = 0.0;
    for (double t : target) tMax = std::max(tMax, t);
    std::vector<double> weight(omegas.size(), 1.0);
    for (size_t k = 0; k < omegas.size(); ++k)
        if (!(target[k] >= tMax * kFitRangePower)) weight[k] = 0.0;

    // Analog zeros on the j*Omega axis keep their exact frequency as unit-circle zeros;
    // only the gain is fitted (minimax in dB over the active grid):
    //  - b1 == 0, b0 > 0, b2 > 0: a zero pair at Omega_z = sqrt(b0/b2) (notch);
    //  - b0 == b1 == 0: a double zero at DC -> (1 - z^-1)^2.
    if (!firstOrder) {
        double c = 0.0;
        bool fixed = false;
        if (s.b1 == 0.0 && s.b0 > 0.0 && s.b2 > 0.0) {
            const double omegaZ = std::sqrt(s.b0 / s.b2) * omegaC;
            if (omegaZ < kPi) {
                c = -2.0 * std::cos(omegaZ);
                fixed = true;
            }
        } else if (s.b0 == 0.0 && s.b1 == 0.0 && s.b2 != 0.0) {
            c = -2.0;
            fixed = true;
        }
        if (fixed) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (size_t k = 0; k < omegas.size(); ++k) {
                if (weight[k] == 0.0) continue;
                const double m2 = std::norm(1.0 + c * std::polar(1.0, -omegas[k]) + std::polar(1.0, -2.0 * omegas[k]));
                if (!(m2 > 1e-12) || !(target[k] > 0.0)) continue;
                const double r = std::log(target[k] / m2);
                lo = std::min(lo, r);
                hi = std::max(hi, r);
            }
            // DC exact when the section passes DC (consistent with the general fit);
            // otherwise centre the dB error range.
            const double t0 = analogMag2(s, 0.0, omegaC) * denMag2(0.0);
            const double m0 = (2.0 + c) * (2.0 + c);
            const double g = (t0 > 0.0 && m0 > 1e-12) ? std::sqrt(t0 / m0)
                             : std::isfinite(lo)       ? std::exp(0.25 * (lo + hi))
                                                       : 1.0;
            bq.b0 = g;
            bq.b1 = g * c;
            bq.b2 = g;
            return bq;
        }
    }

    // B0 is fixed so DC is exact (as in the three-point match; zero for a DC zero).
    // Fitted columns: phi1, and phi2 for second-order sections.
    const double B0 = zeroAtDc ? 0.0 : analogMag2(s, 0.0, omegaC) * denMag2(0.0);
    int cols[2];
    int m = 0;
    cols[m++] = 1;
    if (!firstOrder) cols[m++] = 2;
    const double tFloor = tMax * kFitRangePower;

    double B[3] = {B0, 0.0, 0.0};
    std::vector<std::array<double, 3>> A(omegas.size());
    std::vector<double> y(omegas.size());
    constexpr int kLawsonIterations = 12;
    for (int it = 0; it < kLawsonIterations; ++it) {
        for (size_t k = 0; k < omegas.size(); ++k) {
            const Phi ph = phiAt(omegas[k]);
            const double basis[3] = {ph.p0, ph.p1, ph.p2};
            const double t = std::max(target[k], tFloor);
            const double sw = std::sqrt(weight[k]);
            for (int j = 0; j < m; ++j) A[k][static_cast<size_t>(j)] = sw * basis[cols[j]] / t;
            y[k] = sw * (1.0 - B0 * ph.p0 / t);
        }
        double x[3] = {0.0, 0.0, 0.0};
        if (!leastSquares(A, y, m, x)) break;
        B[1] = B[2] = 0.0;
        for (int j = 0; j < m; ++j) B[cols[j]] = x[j];
        // Lawson update: w <- w * |relative error|, renormalised.
        double wsum = 0.0;
        for (size_t k = 0; k < omegas.size(); ++k) {
            const Phi ph = phiAt(omegas[k]);
            const double n = B[0] * ph.p0 + B[1] * ph.p1 + B[2] * ph.p2;
            const double t = std::max(target[k], tFloor);
            if (weight[k] > 0.0) weight[k] *= std::abs(n / t - 1.0) + 1e-12;
            wsum += weight[k];
        }
        if (!(wsum > 0.0)) break;
        for (auto& w : weight) w *= static_cast<double>(weight.size()) / wsum;
    }

    if (firstOrder) {
        const double W = std::sqrt(std::max(B[0], 0.0));
        const double V = std::sqrt(std::max(B[1], 0.0));
        bq.b0 = 0.5 * (W + V);
        bq.b1 = 0.5 * (W - V);
        bq.b2 = 0.0;
        return bq;
    }
    if (numeratorFromB(B[0], B[1], B[2], bq)) return bq;
    clamped = true;
    const double P = 0.5 * (std::sqrt(std::max(B[0], 0.0)) + std::sqrt(std::max(B[1], 0.0)));
    numeratorFromB(B[0], B[1], -P * P, bq);
    return bq;
}

Biquad bilinearSection(const AnalogSection& s, double K) {
    // s = K (1 - z^-1) / (1 + z^-1); multiply through by (1 + z^-1)^2.
    const double K2 = K * K;
    const double n0 = s.b0 + s.b1 * K + s.b2 * K2;
    const double n1 = 2.0 * s.b0 - 2.0 * s.b2 * K2;
    const double n2 = s.b0 - s.b1 * K + s.b2 * K2;
    const double d0 = s.a0 + s.a1 * K + s.a2 * K2;
    const double d1 = 2.0 * s.a0 - 2.0 * s.a2 * K2;
    const double d2 = s.a0 - s.a1 * K + s.a2 * K2;
    Biquad bq;
    bq.b0 = n0 / d0;
    bq.b1 = n1 / d0;
    bq.b2 = n2 / d0;
    bq.a1 = d1 / d0;
    bq.a2 = d2 / d0;
    return bq;
}

}  // namespace

std::complex<double> Biquad::response(double omega) const noexcept {
    const cd z1 = std::polar(1.0, -omega);
    const cd z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

std::complex<double> DigitalFilter::response(double omega) const noexcept {
    cd h = 1.0;
    for (const auto& s : sections) h *= s.response(omega);
    return h;
}

std::complex<double> DigitalFilter::responseHz(double hz) const noexcept {
    return response(hzToRadiansPerSample(hz, sampleRate));
}

DigitalFilter designMatched(const AnalogPrototype& proto, double sampleRate, DesignMethod method) {
    DigitalFilter f;
    f.sampleRate = sampleRate;
    if (!proto.supported) return f;
    const double omegaC = hzToRadiansPerSample(proto.centreHz, sampleRate);
    f.sections.reserve(proto.sections.size());
    for (const auto& s : proto.sections) {
        bool clamped = false;
        f.sections.push_back(method == DesignMethod::MatchedThreePoint ? matchSection(s, omegaC, clamped)
                                                                       : fitSection(s, omegaC, clamped));
        if (clamped) ++f.clampedSections;
    }
    if (method != DesignMethod::MatchedRefined) return f;
    return designRefined(proto, sampleRate, RefineOptions{});
}

double r12ToleranceDb(double centreHz, double q, double sampleRate) noexcept {
    const double nyquist = 0.5 * sampleRate;
    if (centreHz <= 0.9 * nyquist) return 0.1;
    if (q <= 3.0) return 1.1;
    return std::numeric_limits<double>::quiet_NaN();  // TBD until R14
}

double r12DesignTargetDb(double centreHz, double q, double sampleRate) noexcept {
    const double t = r12ToleranceDb(centreHz, q, sampleRate);
    return std::isnan(t) ? 1.1 : t;
}

DigitalFilter designRefined(const AnalogPrototype& proto, double sampleRate, const RefineOptions& opts) {
    // Joint refinement is a local optimisation, so it is run from several starting
    // designs (least-squares matched, three-point matched, pre-warped bilinear) at the
    // analog order. Candidates are compared on an independent dense check grid. If
    // extra correction sections are allowed and the target is still missed, the best
    // order-N result is refined again with identity sections appended, so an N+2k
    // design is never worse than the order-N one.
    const DigitalFilter ls = designMatched(proto, sampleRate, DesignMethod::MatchedLeastSquares);
    if (!proto.supported) return ls;
    RefineOptions base = opts;
    base.extraSections = 0;
    DigitalFilter best = ls;
    double bestErr = denseCheckMaxErrorDb(proto, ls, base);
    auto consider = [&](const DigitalFilter& cand) {
        const double e = denseCheckMaxErrorDb(proto, cand, base);
        if (e < bestErr) {
            bestErr = e;
            best = cand;
        }
    };
    if (bestErr > base.stopDb) consider(refineJoint(proto, ls, base));
    if (bestErr > base.stopDb) consider(refineJoint(proto, designMatched(proto, sampleRate, DesignMethod::MatchedThreePoint), base));
    if (bestErr > base.stopDb) consider(refineJoint(proto, designBilinear(proto, sampleRate), base));
    if (opts.extraSections > 0) {
        const double target = opts.extraOnlyWhenNeeded
                                  ? opts.extraTriggerFraction * r12DesignTargetDb(proto.centreHz, proto.q, sampleRate)
                                  : base.stopDb;
        if (bestErr > target) {
            RefineOptions extra = opts;
            extra.startDb = 0.0;  // always refine: the extra sections start as identity
            consider(refineJoint(proto, best, extra));
        }
    }
    return best;
}

DigitalFilter designBilinear(const AnalogPrototype& proto, double sampleRate, double prewarpHz) {
    DigitalFilter f;
    f.sampleRate = sampleRate;
    if (!proto.supported) return f;
    double fw = prewarpHz > 0.0 ? prewarpHz : proto.centreHz;
    fw = std::min(fw, 0.95 * 0.5 * sampleRate);
    const double omegaW = hzToRadiansPerSample(fw, sampleRate);
    const double K = (fw / proto.centreHz) / std::tan(0.5 * omegaW);
    f.sections.reserve(proto.sections.size());
    for (const auto& s : proto.sections) f.sections.push_back(bilinearSection(s, K));
    return f;
}

DigitalFilter designDigital(const AnalogPrototype& proto, double sampleRate, DesignMethod method) {
    if (proto.shape == Shape::AllPass || method == DesignMethod::BilinearPrewarp)
        return designBilinear(proto, sampleRate);
    return designMatched(proto, sampleRate, method);
}

}  // namespace amq::dsp
