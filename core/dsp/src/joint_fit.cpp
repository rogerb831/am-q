// Joint minimax refinement of a cascaded-biquad design against its analog prototype.
//
// Technique: weighted nonlinear least squares on the dB magnitude error, solved by
// Levenberg-Marquardt (K. Levenberg 1944; D. W. Marquardt 1963), with Lawson's
// iteratively re-weighted least squares (C. L. Lawson 1961) pushing the weighted L2
// solution towards the minimax error. Magnitude-only optimisation of cascaded
// second-order sections is the "minimum p-error" approach of A. G. Deczky, "Synthesis
// of recursive digital filters using the minimum p-error criterion", IEEE Trans.
// Audio Electroacoust. AU-20(4), 1972. Nothing here is specific to any product.

#include "amq/dsp/conventions.hpp"
#include "amq/dsp/digital_design.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace amq::dsp {
namespace {

using cd = std::complex<double>;
constexpr double kDbPerNeper = 20.0 / 2.302585092994046;  // 20 / ln(10)

enum class Kind {
    InBand,          // below the limit, analog above the floor: e = D - T
    InBandBelowFloor,// below the limit, analog below the floor: e = max(0, D - floor)
    AboveLimit,      // between the limit and Nyquist: loose dead-zone tracking
};

struct FitPoint {
    double omega;
    double targetDb;
    Kind kind;
};

// Fit grid. Log-spaced over the whole band, plus a medium-density window around f0
// and a fine window whose step shrinks with Q (narrow peaks and notches).
std::vector<FitPoint> buildGrid(const AnalogPrototype& proto, double fs, const RefineOptions& o, double q) {
    const double nyq = 0.5 * fs;
    const double limit = o.limitFraction * nyq;
    const double lo = std::min(kMinBandFrequencyHz, 0.5 * limit);
    std::vector<double> hz;
    constexpr int kLog = 200;
    for (int i = 0; i < kLog; ++i) hz.push_back(lo * std::pow(limit / lo, i / double(kLog - 1)));
    const double f0 = proto.centreHz;
    for (double oct = -2.5; oct <= 2.5; oct += 1.0 / 32.0) hz.push_back(f0 * std::exp2(oct + 1.0 / 97.0));
    const double fine = std::min(1.0 / 32.0, 1.0 / (12.0 * std::max(q, 0.025)));
    const double span = std::max(0.25, 6.0 / std::max(q, 0.025));
    if (fine < 1.0 / 32.0)
        for (double oct = -span; oct <= span; oct += fine) hz.push_back(f0 * std::exp2(oct));
    // Very fine points around notch zeros (jOmega-axis zeros), where the response
    // crosses many dB in a narrow band.
    for (const auto& s : proto.sections) {
        if (!(s.b1 == 0.0 && s.b0 > 0.0 && s.b2 > 0.0)) continue;
        const double fz = std::sqrt(s.b0 / s.b2) * f0;
        for (double oct = -0.15; oct <= 0.15; oct += 1.0 / 600.0) hz.push_back(fz * std::exp2(oct));
    }
    std::vector<FitPoint> pts;
    pts.reserve(hz.size() + 24);
    for (double f : hz) {
        if (f < lo || f > limit) continue;
        const double t = linearToDb(std::abs(proto.evaluateHz(f)));
        pts.push_back({hzToRadiansPerSample(f, fs), t, t < o.floorDb ? Kind::InBandBelowFloor : Kind::InBand});
    }
    for (int i = 1; i <= 16; ++i) {  // (limit, Nyquist]
        const double f = limit + (nyq - limit) * i / 16.0;
        const double t = linearToDb(std::abs(proto.evaluateHz(f)));
        pts.push_back({hzToRadiansPerSample(f, fs), std::max(t, o.floorDb), Kind::AboveLimit});
    }
    return pts;
}

// Per-section parameter model. The numerator is b = M * p (M is 3 x k), so structural
// zeros of the analog section stay exact during the joint fit:
//  - jOmega-axis zero pair below Nyquist (notch): b = g [1, -2cos(wz), 1]       (k = 1)
//  - double zero at DC:                            b = g [1, -2, 1]              (k = 1)
//  - single zero at DC: (1 - z^-1)(c0 + c1 z^-1) = [c0, c1 - c0, -c1]            (k = 2)
//  - otherwise free b0, b1, b2 (k = 3), or b0, b1 for first-order sections (k = 2).
// The denominator is a1 (first order) or a1, a2.
struct SectionModel {
    double M[3][3] = {};
    int k = 3;
    bool firstOrderDen = false;
    int offset = 0;
};

struct Layout {
    std::vector<SectionModel> sec;
    int count = 0;
};

Layout makeLayout(const AnalogPrototype& proto, const DigitalFilter& d, int extraSections) {
    Layout l;
    const double omegaC = hzToRadiansPerSample(proto.centreHz, d.sampleRate);
    for (size_t i = 0; i < d.sections.size(); ++i) {
        SectionModel m;
        const AnalogSection* a = i < proto.sections.size() ? &proto.sections[i] : nullptr;
        const bool first = a ? (a->a2 == 0.0 && a->b2 == 0.0) : false;
        m.firstOrderDen = first;
        if (a && first) {
            if (a->b0 == 0.0) { m.k = 1; m.M[0][0] = 1.0; m.M[1][0] = -1.0; }
            else { m.k = 2; m.M[0][0] = 1.0; m.M[1][1] = 1.0; }
        } else if (a && a->b0 == 0.0 && a->b1 == 0.0) {
            m.k = 1; m.M[0][0] = 1.0; m.M[1][0] = -2.0; m.M[2][0] = 1.0;
        } else if (a && a->b0 == 0.0) {
            m.k = 2; m.M[0][0] = 1.0; m.M[1][0] = -1.0; m.M[1][1] = 1.0; m.M[2][1] = -1.0;
        } else if (a && a->b1 == 0.0 && a->b0 > 0.0 && a->b2 > 0.0 &&
                   std::sqrt(a->b0 / a->b2) * omegaC < 0.999 * kPi) {
            const double wz = std::sqrt(a->b0 / a->b2) * omegaC;
            m.k = 1; m.M[0][0] = 1.0; m.M[1][0] = -2.0 * std::cos(wz); m.M[2][0] = 1.0;
        } else {
            m.k = 3; m.M[0][0] = 1.0; m.M[1][1] = 1.0; m.M[2][2] = 1.0;
        }
        m.offset = l.count;
        l.count += m.k + (m.firstOrderDen ? 1 : 2);
        l.sec.push_back(m);
    }
    for (int e = 0; e < extraSections; ++e) {
        SectionModel m;
        m.k = 3; m.M[0][0] = 1.0; m.M[1][1] = 1.0; m.M[2][2] = 1.0;
        m.offset = l.count;
        l.count += 5;
        l.sec.push_back(m);
    }
    return l;
}

// Projects b onto the section's numerator model (least squares over the k columns).
void projectNumerator(const SectionModel& m, const Biquad& b, double* p) {
    const double bv[3] = {b.b0, b.b1, b.b2};
    double G[3][3] = {}, r[3] = {};
    for (int a = 0; a < m.k; ++a) {
        for (int c = 0; c < m.k; ++c)
            for (int t = 0; t < 3; ++t) G[a][c] += m.M[t][a] * m.M[t][c];
        for (int t = 0; t < 3; ++t) r[a] += m.M[t][a] * bv[t];
    }
    // Small Gaussian elimination.
    for (int c = 0; c < m.k; ++c) {
        for (int rr = c + 1; rr < m.k; ++rr) {
            const double f = G[rr][c] / G[c][c];
            for (int t = c; t < m.k; ++t) G[rr][t] -= f * G[c][t];
            r[rr] -= f * r[c];
        }
    }
    for (int c = m.k - 1; c >= 0; --c) {
        double v = r[c];
        for (int t = c + 1; t < m.k; ++t) v -= G[c][t] * p[t];
        p[c] = v / G[c][c];
    }
}

std::vector<double> pack(const DigitalFilter& d, const Layout& l) {
    std::vector<double> x(static_cast<size_t>(l.count), 0.0);
    for (size_t i = 0; i < l.sec.size(); ++i) {
        const auto& m = l.sec[i];
        double* p = &x[static_cast<size_t>(m.offset)];
        const Biquad b = i < d.sections.size() ? d.sections[i] : Biquad{};
        projectNumerator(m, b, p);
        p[m.k] = b.a1;
        if (!m.firstOrderDen) p[m.k + 1] = b.a2;
    }
    return x;
}

void unpack(const std::vector<double>& x, const Layout& l, DigitalFilter& d) {
    d.sections.resize(l.sec.size());
    for (size_t i = 0; i < l.sec.size(); ++i) {
        const auto& m = l.sec[i];
        const double* p = &x[static_cast<size_t>(m.offset)];
        auto& s = d.sections[i];
        double b[3] = {0.0, 0.0, 0.0};
        for (int t = 0; t < 3; ++t)
            for (int j = 0; j < m.k; ++j) b[t] += m.M[t][j] * p[j];
        s.b0 = b[0]; s.b1 = b[1]; s.b2 = b[2];
        s.a1 = p[m.k];
        s.a2 = m.firstOrderDen ? 0.0 : p[m.k + 1];
    }
}

// Strictly stable (poles inside the unit circle, with a tiny margin).
bool stable(const DigitalFilter& d) {
    constexpr double m = 1e-12;
    for (const auto& s : d.sections) {
        if (!std::isfinite(s.a1) || !std::isfinite(s.a2)) return false;
        if (s.a2 == 0.0) {
            if (std::abs(s.a1) >= 1.0 - m) return false;
        } else if (!(std::abs(s.a2) < 1.0 - m && std::abs(s.a1) < 1.0 + s.a2 - m)) {
            return false;
        }
    }
    return true;
}

double residual(const FitPoint& p, double dDb, const RefineOptions& o, double* slope) {
    // Returns e and sets *slope = de/dD.
    switch (p.kind) {
        case Kind::InBand:
            *slope = 1.0;
            return dDb - p.targetDb;
        case Kind::InBandBelowFloor: {
            const double t = o.floorDb - 1.0;
            if (dDb > t) { *slope = 1.0; return dDb - t; }
            *slope = 0.0;
            return 0.0;
        }
        case Kind::AboveLimit: {
            constexpr double dead = 3.0;
            const double e = dDb - p.targetDb;
            if (p.targetDb <= o.floorDb && e < 0.0) { *slope = 0.0; return 0.0; }
            if (e > dead) { *slope = 1.0; return e - dead; }
            if (e < -dead) { *slope = 1.0; return e + dead; }
            *slope = 0.0;
            return 0.0;
        }
    }
    *slope = 0.0;
    return 0.0;
}

double bandErrorDb(const FitPoint& p, double dDb, double floorDb) {
    if (p.kind == Kind::AboveLimit) return 0.0;
    return std::abs(std::max(dDb, floorDb) - std::max(p.targetDb, floorDb));
}

double maxBandError(const DigitalFilter& d, const std::vector<FitPoint>& grid, double floorDb,
                    std::vector<double>* errs = nullptr) {
    double m = 0.0;
    if (errs) errs->assign(grid.size(), 0.0);
    for (size_t k = 0; k < grid.size(); ++k) {
        const double dDb = linearToDb(std::abs(d.response(grid[k].omega)));
        double e = bandErrorDb(grid[k], dDb, floorDb);
        if (!std::isfinite(e)) e = 1e9;
        if (errs) (*errs)[k] = e;
        m = std::max(m, e);
    }
    return m;
}

// Solves (A) x = b in place for a small dense symmetric system (Gaussian elimination
// with partial pivoting). Returns false if singular.
bool solveDense(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (std::abs(A[r * n + c]) > std::abs(A[piv * n + c])) piv = r;
        if (!(std::abs(A[piv * n + c]) > 0.0)) return false;
        if (piv != c) {
            for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            if (f == 0.0) continue;
            for (int k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double v = b[r];
        for (int k = r + 1; k < n; ++k) v -= A[r * n + k] * b[k];
        b[r] = v / A[r * n + r];
    }
    return true;
}

// Reflects numerator zeros outside the unit circle to make each section minimum phase
// (magnitude unchanged).
void makeMinimumPhase(DigitalFilter& d) {
    for (auto& s : d.sections) {
        if (s.b2 == 0.0) {
            if (s.b0 != 0.0 && std::abs(s.b1) > std::abs(s.b0)) std::swap(s.b0, s.b1);  // |b0 + b1 z^-1| = |b1 + b0 z^-1|
            continue;
        }
        // Roots of b0 z^2 + b1 z + b2.
        const cd disc = std::sqrt(cd{s.b1 * s.b1 - 4.0 * s.b0 * s.b2, 0.0});
        if (s.b0 == 0.0) continue;
        cd r1 = (-s.b1 + disc) / (2.0 * s.b0);
        cd r2 = (-s.b1 - disc) / (2.0 * s.b0);
        double g = s.b0;
        auto reflect = [&g](cd& r) {
            const double m = std::abs(r);
            if (m > 1.0) {
                g *= m;
                r = 1.0 / std::conj(r);
            }
        };
        reflect(r1);
        reflect(r2);
        s.b0 = g;
        s.b1 = -g * (r1 + r2).real();
        s.b2 = g * (r1 * r2).real();
    }
}

}  // namespace

// Highest pole Q among the prototype's second-order sections (sets fine-grid density).
static double prototypeMaxQ(const AnalogPrototype& proto) {
    double qMax = 0.5;
    for (const auto& s : proto.sections)
        if (s.a2 != 0.0 && s.a1 > 0.0) qMax = std::max(qMax, std::sqrt(s.a0 * s.a2) / s.a1);
    return qMax;
}

double denseCheckMaxErrorDb(const AnalogPrototype& proto, const DigitalFilter& d, const RefineOptions& o) {
    // Independent of the fit grid: 4096 log points over the band, 1/96 octave within
    // +/-3 octaves of f0, a Q-scaled fine window and the notch windows (offset from the
    // fit grid's points so spikes between fit points are caught).
    const double fs = d.sampleRate;
    const double limit = o.limitFraction * 0.5 * fs;
    const double lo = std::min(kMinBandFrequencyHz, 0.5 * limit);
    const double f0 = proto.centreHz;
    double worst = 0.0;
    auto check = [&](double hz) {
        if (hz < lo || hz > limit) return;
        const double a = linearToDbFloored(std::abs(proto.evaluateHz(hz)), o.floorDb);
        const double g = linearToDbFloored(std::abs(d.responseHz(hz)), o.floorDb);
        const double e = std::abs(a - g);
        worst = std::isfinite(e) ? std::max(worst, e) : 1e9;
    };
    for (int i = 0; i < 4096; ++i) check(lo * std::pow(limit / lo, (i + 0.37) / 4096.0));
    for (double oct = -3.0; oct <= 3.0; oct += 1.0 / 96.0) check(f0 * std::exp2(oct + 1.0 / 211.0));
    const double q = prototypeMaxQ(proto);
    const double fine = 1.0 / (20.0 * q);
    if (fine < 1.0 / 96.0)
        for (double oct = -std::max(0.3, 8.0 / q); oct <= std::max(0.3, 8.0 / q); oct += fine) check(f0 * std::exp2(oct + fine / 3.0));
    for (const auto& s : proto.sections) {
        if (!(s.b1 == 0.0 && s.b0 > 0.0 && s.b2 > 0.0)) continue;
        const double fz = std::sqrt(s.b0 / s.b2) * f0;
        for (double oct = -0.15; oct <= 0.15; oct += 1.0 / 1000.0) check(fz * std::exp2(oct + 1.0 / 3001.0));
    }
    return worst;
}

double fitGridMaxErrorDb(const AnalogPrototype& proto, const DigitalFilter& d, const RefineOptions& o) {
    const auto grid = buildGrid(proto, d.sampleRate, o, prototypeMaxQ(proto));
    return maxBandError(d, grid, o.floorDb);
}

DigitalFilter refineJoint(const AnalogPrototype& proto, const DigitalFilter& start, const RefineOptions& o) {
    if (!proto.supported || start.sections.empty()) return start;

    const auto grid = buildGrid(proto, start.sampleRate, o, prototypeMaxQ(proto));
    const size_t m = grid.size();

    const double startErr = maxBandError(start, grid, o.floorDb);
    if (!(startErr > o.startDb)) return start;

    DigitalFilter cur = start;
    const Layout layout = makeLayout(proto, cur, o.extraSections);
    cur.sections.resize(layout.sec.size());
    const int n = layout.count;
    std::vector<double> x = pack(cur, layout);

    DigitalFilter best = start;
    double bestErr = startErr;

    std::vector<double> w(m, 1.0);
    for (size_t k = 0; k < m; ++k)
        if (grid[k].kind == Kind::AboveLimit) w[k] = 0.1;

    std::vector<double> J(m * static_cast<size_t>(n));
    std::vector<double> r(m);
    std::vector<double> H(static_cast<size_t>(n) * n), g(static_cast<size_t>(n));
    std::vector<double> errs;

    // Residuals and Jacobian at x (weighted). Returns the weighted cost.
    auto evaluate = [&](const std::vector<double>& xv, bool withJacobian) {
        DigitalFilter d = cur;
        unpack(xv, layout, d);
        double cost = 0.0;
        for (size_t k = 0; k < m; ++k) {
            const double om = grid[k].omega;
            const cd z1 = std::polar(1.0, -om);
            const cd z2 = z1 * z1;
            double dDb = 0.0;
            // Per-section numerator/denominator values (reused for the Jacobian).
            for (size_t i = 0; i < d.sections.size(); ++i) {
                const auto& s = d.sections[i];
                const cd N = s.b0 + s.b1 * z1 + s.b2 * z2;
                const cd A = 1.0 + s.a1 * z1 + s.a2 * z2;
                dDb += kDbPerNeper * (std::log(std::abs(N)) - std::log(std::abs(A)));
                if (withJacobian) {
                    const auto& sm = layout.sec[i];
                    double* row = &J[k * static_cast<size_t>(n) + static_cast<size_t>(sm.offset)];
                    const cd iN = 1.0 / N, iA = 1.0 / A;
                    const double dB[3] = {kDbPerNeper * iN.real(), kDbPerNeper * (z1 * iN).real(),
                                          kDbPerNeper * (z2 * iN).real()};
                    for (int j = 0; j < sm.k; ++j)
                        row[j] = dB[0] * sm.M[0][j] + dB[1] * sm.M[1][j] + dB[2] * sm.M[2][j];
                    row[sm.k] = -kDbPerNeper * (z1 * iA).real();
                    if (!sm.firstOrderDen) row[sm.k + 1] = -kDbPerNeper * (z2 * iA).real();
                }
            }
            double slope = 0.0;
            double e = residual(grid[k], dDb, o, &slope);
            if (!std::isfinite(e)) { e = 1e6; slope = 0.0; }
            const double sw = std::sqrt(w[k]);
            r[k] = sw * e;
            cost += r[k] * r[k];
            if (withJacobian) {
                double* row = &J[k * static_cast<size_t>(n)];
                const double f = sw * slope;
                for (int j = 0; j < n; ++j) row[j] = std::isfinite(row[j]) ? row[j] * f : 0.0;
            }
        }
        return cost;
    };

    for (int outer = 0; outer < o.maxOuter; ++outer) {
        double lambda = 1e-3;
        double cost = evaluate(x, true);
        for (int inner = 0; inner < o.maxInner; ++inner) {
            // Normal equations.
            std::fill(H.begin(), H.end(), 0.0);
            std::fill(g.begin(), g.end(), 0.0);
            for (size_t k = 0; k < m; ++k) {
                const double* row = &J[k * static_cast<size_t>(n)];
                if (r[k] == 0.0 && row[0] == 0.0) {
                    bool allZero = true;
                    for (int j = 0; j < n && allZero; ++j) allZero = row[j] == 0.0;
                    if (allZero) continue;
                }
                for (int a = 0; a < n; ++a) {
                    if (row[a] == 0.0) continue;
                    g[a] += row[a] * r[k];
                    for (int b = a; b < n; ++b) H[a * n + b] += row[a] * row[b];
                }
            }
            for (int a = 0; a < n; ++a)
                for (int b = 0; b < a; ++b) H[a * n + b] = H[b * n + a];
            double maxDiag = 0.0;
            for (int a = 0; a < n; ++a) maxDiag = std::max(maxDiag, H[a * n + a]);
            if (!(maxDiag > 0.0)) break;

            bool accepted = false;
            for (int attempt = 0; attempt < 12; ++attempt) {
                std::vector<double> A = H;
                std::vector<double> step(g.size());
                for (int a = 0; a < n; ++a) {
                    A[a * n + a] += lambda * (H[a * n + a] + 1e-9 * maxDiag);
                    step[a] = -g[a];
                }
                if (!solveDense(A, step, n)) {
                    lambda *= 10.0;
                    continue;
                }
                std::vector<double> xn = x;
                for (int a = 0; a < n; ++a) xn[a] += step[a];
                DigitalFilter trial = cur;
                unpack(xn, layout, trial);
                if (!stable(trial)) {
                    lambda *= 4.0;
                    continue;
                }
                const double newCost = evaluate(xn, false);
                if (newCost < cost) {
                    x = xn;
                    lambda = std::max(lambda / 3.0, 1e-12);
                    const double prev = cost;
                    cost = evaluate(x, true);
                    accepted = true;
                    if (prev - cost < 1e-10 * prev) inner = o.maxInner;  // converged
                    break;
                }
                lambda *= 4.0;
            }
            if (!accepted) break;
        }

        unpack(x, layout, cur);
        const double err = maxBandError(cur, grid, o.floorDb, &errs);
        if (err < bestErr) {
            bestErr = err;
            best = cur;
        }
        if (bestErr <= o.stopDb) break;

        // Lawson re-weighting on the in-band points.
        double sum = 0.0;
        size_t cnt = 0;
        for (size_t k = 0; k < m; ++k) {
            if (grid[k].kind == Kind::AboveLimit) continue;
            w[k] *= errs[k] + 1e-4 * err;
            sum += w[k];
            ++cnt;
        }
        if (!(sum > 0.0)) break;
        for (size_t k = 0; k < m; ++k)
            if (grid[k].kind != Kind::AboveLimit) w[k] *= static_cast<double>(cnt) / sum;
    }

    makeMinimumPhase(best);
    best.clampedSections = start.clampedSections;
    return best;
}

}  // namespace amq::dsp
