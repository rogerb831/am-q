#include "amq/dsp/analog_prototype.hpp"

#include "amq/dsp/conventions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace amq::dsp {
namespace {

using cd = std::complex<double>;

// Zero/pole/gain form: H(s) = k * prod(s - z) / prod(s - p). Internal only.
struct Zpk {
    std::vector<cd> zeros;
    std::vector<cd> poles;
    double k = 1.0;
};

// Two roots of s^2 + (r/Q) s + r^2 (a pole or zero pair of radius r and quality Q).
void appendPairFromRadiusQ(std::vector<cd>& out, double r, double q) {
    const double zeta = 1.0 / (2.0 * q);
    if (zeta < 1.0) {
        const double im = std::sqrt(1.0 - zeta * zeta);
        out.emplace_back(-zeta * r, im * r);
        out.emplace_back(-zeta * r, -im * r);
    } else {
        const double d = std::sqrt(zeta * zeta - 1.0);
        out.emplace_back((-zeta + d) * r, 0.0);
        out.emplace_back((-zeta - d) * r, 0.0);
    }
}

// Unit-radius Butterworth poles of order m. The dominant (highest-Q) pair has its Q
// multiplied by qScale (PROVISIONAL Q convention, see qMappingNotes()).
std::vector<cd> butterworthPoles(int m, double qScale) {
    std::vector<cd> p;
    for (int i = 0; i < m / 2; ++i) {
        double q = 1.0 / (2.0 * std::sin((2.0 * i + 1.0) * kPi / (2.0 * m)));
        if (i == 0) q *= qScale;
        appendPairFromRadiusQ(p, 1.0, q);
    }
    if (m % 2 == 1) p.emplace_back(-1.0, 0.0);
    return p;
}

double realProduct(const std::vector<cd>& v, double sign) {
    cd prod = 1.0;
    for (const auto& x : v) prod *= sign * x;
    return prod.real();
}

Zpk lowpass(int m, double qScale) {
    Zpk z;
    z.poles = butterworthPoles(m, qScale);
    z.k = realProduct(z.poles, -1.0);  // unity DC gain
    return z;
}

Zpk highpass(int m, double qScale) {
    Zpk z;
    z.poles = butterworthPoles(m, qScale);
    z.zeros.assign(static_cast<size_t>(m), cd{0.0, 0.0});
    z.k = 1.0;  // unity high-frequency gain
    return z;
}

// Butterworth low shelf: DC gain g (linear), HF gain 1, gain sqrt(g) at Omega = 1.
// Poles on radius g^(-1/2m), zeros on radius g^(+1/2m), same angles (Orfanidis 2005).
Zpk lowShelf(int m, double g, double qScale) {
    Zpk z;
    const auto unit = butterworthPoles(m, qScale);
    const double rz = std::pow(g, 0.5 / m);
    const double rp = 1.0 / rz;
    for (const auto& u : unit) {
        z.zeros.push_back(u * rz);
        z.poles.push_back(u * rp);
    }
    z.k = 1.0;
    return z;
}

// High shelf with gain g = g * lowShelf(1/g).
Zpk highShelf(int m, double g, double qScale) {
    Zpk z = lowShelf(m, 1.0 / g, qScale);
    z.k *= g;
    return z;
}

// Low-pass to band-pass mapping s -> Qb (s + 1/s), centre Omega = 1 (Orfanidis 2005;
// standard analog frequency transformation). Doubles the order.
Zpk lowpassToBandpass(const Zpk& in, double qb) {
    Zpk out;
    auto map = [qb](const cd& r, std::vector<cd>& dst) {
        const cd half = r / (2.0 * qb);
        const cd root = std::sqrt(half * half - 1.0);
        dst.push_back(half + root);
        dst.push_back(half - root);
    };
    for (const auto& z : in.zeros) map(z, out.zeros);
    for (const auto& p : in.poles) map(p, out.poles);
    const int degree = static_cast<int>(in.poles.size()) - static_cast<int>(in.zeros.size());
    for (int i = 0; i < degree; ++i) out.zeros.emplace_back(0.0, 0.0);
    out.k = in.k * std::pow(1.0 / qb, degree);
    return out;
}

// All pass: Butterworth poles, zeros mirrored into the right half plane.
Zpk allPass(int m, double qScale) {
    Zpk z;
    z.poles = butterworthPoles(m, qScale);
    for (const auto& p : z.poles) z.zeros.push_back(-std::conj(p));
    z.k = 1.0;
    return z;
}

// A real polynomial factor of degree 1 or 2 built from its roots.
struct Factor {
    int degree = 0;
    cd rep;       // representative root (upper half plane for complex pairs)
    double c0 = 1.0, c1 = 0.0, c2 = 0.0;  // c0 + c1 s + c2 s^2, monic in the top power
    double q = 0.0;                       // pole quality, for ordering
};

std::vector<Factor> groupIntoFactors(const std::vector<cd>& roots) {
    std::vector<Factor> out;
    std::vector<double> reals;
    for (const auto& r : roots) {
        const double tol = 1e-12 * std::max(1.0, std::abs(r));
        if (r.imag() > tol) {
            Factor f;
            f.degree = 2;
            f.rep = r;
            f.c2 = 1.0;
            f.c1 = -2.0 * r.real();
            f.c0 = std::norm(r);
            const double re = std::abs(r.real());
            f.q = re > 0.0 ? std::abs(r) / (2.0 * re) : std::numeric_limits<double>::infinity();
            out.push_back(f);
        } else if (std::abs(r.imag()) <= tol) {
            reals.push_back(r.real());
        }
        // roots with negative imaginary part are the conjugates of ones already taken
    }
    std::sort(reals.begin(), reals.end());
    size_t i = 0;
    for (; i + 1 < reals.size(); i += 2) {
        Factor f;
        f.degree = 2;
        f.rep = reals[i];
        f.c2 = 1.0;
        f.c1 = -(reals[i] + reals[i + 1]);
        f.c0 = reals[i] * reals[i + 1];
        f.q = 0.0;
        out.push_back(f);
    }
    if (i < reals.size()) {
        Factor f;
        f.degree = 1;
        f.rep = reals[i];
        f.c1 = 1.0;
        f.c0 = -reals[i];
        f.q = -1.0;
        out.push_back(f);
    }
    return out;
}

// Converts zero/pole/gain into cascaded sections. Each pole factor takes the nearest
// unused zero factor of the same degree (falling back to a lower degree), so every
// section stays proper and its dynamic range stays small.
std::vector<AnalogSection> zpkToSections(const Zpk& zpk) {
    auto poleFactors = groupIntoFactors(zpk.poles);
    auto zeroFactors = groupIntoFactors(zpk.zeros);
    std::stable_sort(poleFactors.begin(), poleFactors.end(),
                     [](const Factor& a, const Factor& b) { return a.q > b.q; });

    std::vector<bool> used(zeroFactors.size(), false);
    std::vector<AnalogSection> sections;
    sections.reserve(poleFactors.size());
    for (const auto& pf : poleFactors) {
        int best = -1;
        double bestCost = std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < zeroFactors.size(); ++j) {
            if (used[j] || zeroFactors[j].degree > pf.degree) continue;
            const double mismatch = zeroFactors[j].degree == pf.degree ? 0.0 : 1e6;
            const double cost = std::abs(zeroFactors[j].rep - pf.rep) + mismatch;
            if (cost < bestCost) {
                bestCost = cost;
                best = static_cast<int>(j);
            }
        }
        AnalogSection s;
        s.a0 = pf.c0;
        s.a1 = pf.c1;
        s.a2 = pf.degree == 2 ? pf.c2 : 0.0;
        if (best >= 0) {
            used[static_cast<size_t>(best)] = true;
            const auto& zf = zeroFactors[static_cast<size_t>(best)];
            s.b0 = zf.c0;
            s.b1 = zf.c1;
            s.b2 = zf.degree == 2 ? zf.c2 : 0.0;
        } else {
            s.b0 = 1.0;
            s.b1 = 0.0;
            s.b2 = 0.0;
        }
        sections.push_back(s);
    }
    if (sections.empty()) sections.push_back(AnalogSection{});  // pure gain
    sections[0].b0 *= zpk.k;
    sections[0].b1 *= zpk.k;
    sections[0].b2 *= zpk.k;
    return sections;
}

// PROVISIONAL Flat Tilt: a constant dB/octave tilt between 20 Hz and 20 kHz, built from
// first-order pole/zero pairs spaced half an octave apart (the classic interlaced
// pole/zero approximation of a fractional-slope response). The gain parameter is the
// total tilt across 20 Hz - 20 kHz; the pivot (0 dB) is at the band frequency; the gain
// at the 1 kHz reference is limited to +/-30 dB (R14 "fixed mid-band reference").
inline constexpr double kFlatTiltLowHz = 20.0;
inline constexpr double kFlatTiltHighHz = 20000.0;
inline constexpr double kFlatTiltSpacingOctaves = 0.5;
inline constexpr double kFlatTiltReferenceHz = 1000.0;

Zpk flatTilt(double centreHz, double gainDb) {
    Zpk z;
    if (gainDb == 0.0) return z;
    const double octaves = std::log2(kFlatTiltHighHz / kFlatTiltLowHz);
    const double dbPerOct = gainDb / octaves;
    const double perSectionDb = dbPerOct * kFlatTiltSpacingOctaves;
    const double ratio = dbToLinear(perSectionDb);  // pole/zero frequency ratio
    const int count = static_cast<int>(std::lround(octaves / kFlatTiltSpacingOctaves));
    for (int i = 0; i < count; ++i) {
        const double c = kFlatTiltLowHz * std::exp2((i + 0.5) * kFlatTiltSpacingOctaves) / centreHz;
        const double zf = c / std::sqrt(ratio);
        const double pf = c * std::sqrt(ratio);
        z.zeros.emplace_back(-zf, 0.0);
        z.poles.emplace_back(-pf, 0.0);
    }
    return z;
}

double zpkMagnitude(const Zpk& z, double omega) {
    const cd s{0.0, omega};
    cd h = z.k;
    for (const auto& r : z.zeros) h *= (s - r);
    for (const auto& p : z.poles) h /= (s - p);
    return std::abs(h);
}

}  // namespace

// ---------------------------------------------------------------------------

std::string_view shapeName(Shape s) noexcept {
    switch (s) {
        case Shape::Bell: return "Bell";
        case Shape::LowShelf: return "Low Shelf";
        case Shape::LowCut: return "Low Cut";
        case Shape::HighShelf: return "High Shelf";
        case Shape::HighCut: return "High Cut";
        case Shape::Notch: return "Notch";
        case Shape::BandPass: return "Band Pass";
        case Shape::TiltShelf: return "Tilt Shelf";
        case Shape::FlatTilt: return "Flat Tilt";
        case Shape::AllPass: return "All Pass";
    }
    return "?";
}

std::string_view slopeName(Slope s) noexcept {
    switch (s) {
        case Slope::Db6: return "6";
        case Slope::Db12: return "12";
        case Slope::Db18: return "18";
        case Slope::Db24: return "24";
        case Slope::Db30: return "30";
        case Slope::Db36: return "36";
        case Slope::Db48: return "48";
        case Slope::Db72: return "72";
        case Slope::Db96: return "96";
        case Slope::Brickwall: return "Brickwall";
    }
    return "?";
}

int slopeDbPerOctave(Slope s) noexcept {
    static constexpr int table[] = {6, 12, 18, 24, 30, 36, 48, 72, 96, 0};
    return table[static_cast<int>(s)];
}

bool shapeHasGain(Shape s) noexcept {
    switch (s) {
        case Shape::Bell:
        case Shape::LowShelf:
        case Shape::HighShelf:
        case Shape::TiltShelf:
        case Shape::FlatTilt: return true;
        default: return false;
    }
}

bool shapeUsesQ(Shape s) noexcept { return s != Shape::FlatTilt; }  // PROVISIONAL (R14 tilt Q curve open)

bool shapeUsesSlope(Shape s) noexcept { return s != Shape::FlatTilt; }  // PROVISIONAL

bool slopeAllowed(Shape shape, Slope slope) noexcept {
    if (slope == Slope::Brickwall) return shape == Shape::LowCut || shape == Shape::HighCut;
    return true;
}

int filterOrder(Shape shape, Slope slope) noexcept {
    if (slope == Slope::Brickwall) return 0;
    int n = slopeDbPerOctave(slope) / 6;
    if (shape == Shape::Bell || shape == Shape::BandPass || shape == Shape::Notch) n = std::max(n, 2);
    return n;
}

std::string_view qMappingNotes() noexcept {
    return "PROVISIONAL Q convention (R14 open): "
           "Bell / Notch / Band Pass: displayed Q is the bandwidth Q of the low-pass to band-pass "
           "mapping (centre/bandwidth), prototype order = floor(order/2). "
           "Cuts, shelves, Tilt Shelf, All Pass: Butterworth sections; displayed Q multiplies the "
           "Q of the dominant (highest-Q) pole pair, so Q = 1 is maximally flat; first-order "
           "(6 dB/oct) shapes ignore Q. Flat Tilt ignores Q and slope.";
}

std::complex<double> AnalogSection::evaluateS(std::complex<double> s) const noexcept {
    const auto num = b0 + s * (b1 + s * b2);
    const auto den = a0 + s * (a1 + s * a2);
    return num / den;
}

std::complex<double> AnalogPrototype::evaluate(double omega) const noexcept {
    std::complex<double> h = 1.0;
    for (const auto& s : sections) h *= s.evaluate(omega);
    return h;
}

AnalogPrototype designAnalogPrototype(const BandSpec& specIn) {
    BandSpec spec = specIn;
    spec.gainDb = clampBandGainDb(spec.gainDb);
    spec.q = std::clamp(spec.q, kMinQ, kMaxQ);

    AnalogPrototype proto;
    proto.shape = spec.shape;
    proto.centreHz = spec.frequencyHz;
    proto.order = filterOrder(spec.shape, spec.slope);
    proto.q = spec.q;

    if (spec.slope == Slope::Brickwall || !slopeAllowed(spec.shape, spec.slope)) {
        proto.supported = false;  // Brickwall is a Phase 2 design (area 01 tasks)
        return proto;
    }
    proto.supported = true;

    const int n = proto.order;
    const double g = dbToLinear(spec.gainDb);
    Zpk zpk;
    switch (spec.shape) {
        case Shape::Bell: zpk = lowpassToBandpass(lowShelf(n / 2, g, 1.0), spec.q); break;
        case Shape::Notch: zpk = lowpassToBandpass(highpass(n / 2, 1.0), spec.q); break;
        case Shape::BandPass: zpk = lowpassToBandpass(lowpass(n / 2, 1.0), spec.q); break;
        case Shape::LowShelf: zpk = lowShelf(n, g, spec.q); break;
        case Shape::HighShelf: zpk = highShelf(n, g, spec.q); break;
        case Shape::LowCut: zpk = highpass(n, spec.q); break;
        case Shape::HighCut: zpk = lowpass(n, spec.q); break;
        case Shape::TiltShelf:
            // High shelf of total gain g, re-centred so the pivot (f0) is 0 dB:
            // -g/2 below, +g/2 above.
            zpk = highShelf(n, g, spec.q);
            zpk.k /= std::sqrt(g);
            break;
        case Shape::FlatTilt: {
            zpk = flatTilt(spec.frequencyHz, spec.gainDb);
            proto.order = static_cast<int>(zpk.poles.size());
            if (!zpk.poles.empty()) {
                zpk.k = 1.0 / zpkMagnitude(zpk, 1.0);  // 0 dB at the pivot
                const double refDb = linearToDb(zpkMagnitude(zpk, kFlatTiltReferenceHz / spec.frequencyHz));
                if (refDb > kMaxBandGainDb) zpk.k *= dbToLinear(kMaxBandGainDb - refDb);
                if (refDb < -kMaxBandGainDb) zpk.k *= dbToLinear(-kMaxBandGainDb - refDb);
            }
            break;
        }
        case Shape::AllPass: zpk = allPass(n, spec.q); break;
    }
    proto.sections = zpkToSections(zpk);
    return proto;
}

}  // namespace amq::dsp
