#include "amq/ui/display_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace amq::ui::fmt {

namespace {

std::string printf1(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// A signed number with one decimal: "+" for positive and zero, U+2212 for negative.
std::string signed1(double v) {
    const double r = std::round(v * 10.0) / 10.0;
    const std::string mag = printf1("%.1f", std::abs(r));
    return (r < 0.0 ? std::string(kMinus) : std::string("+")) + mag;
}

}  // namespace

std::string minusFix(const std::string& s) {
    if (!s.empty() && s[0] == '-') return std::string(kMinus) + s.substr(1);
    return s;
}

std::string hz(double v) {
    if (v < 999.5) return printf1("%.0f Hz", v);
    if (v < 9995.0) return printf1("%.2f kHz", v / 1000.0);
    return printf1("%.1f kHz", v / 1000.0);
}

std::string hzAxis(double v) {
    if (v >= 1000.0) return printf1("%gk", v / 1000.0);
    return printf1("%g", v);
}

std::string hzRange(double lo, double hi) {
    const char* dash = "\xE2\x80\x93";  // en dash
    auto part = [](double v) {
        if (v < 1000.0) return printf1("%.0f", v);
        if (v < 10000.0) return printf1("%.1f", v / 1000.0);
        return printf1("%.0f", v / 1000.0);
    };
    const bool k = hi >= 999.5;
    std::string a = k ? (lo < 1000.0 ? printf1("%.1f", lo / 1000.0) : part(lo)) : part(lo);
    return a + dash + part(hi) + (k ? " kHz" : " Hz");
}

std::string db(double v) { return signed1(v) + " dB"; }

std::string dbfs(double v) { return minusFix(printf1("%.1f", std::round(v * 10.0) / 10.0)) + " dB"; }

std::string outputDb(double v, double silenceDb) {
    if (v <= silenceDb) return std::string(kMinus) + "\xE2\x88\x9E dB";  // −∞ dB
    return db(v);
}

std::string q(double v) { return v < 9.995 ? printf1("%.2f", v) : printf1("%.1f", v); }

std::string slopeNumber(int i) {
    static const int kDb[] = {6, 12, 18, 24, 30, 36, 48, 72, 96};
    if (i < 0 || i >= 9) return "Brickwall";
    return std::to_string(kDb[i]);
}

std::string slope(int i) {
    const auto n = slopeNumber(i);
    return n == "Brickwall" ? n : n + " dB/oct";
}

std::string dbPerOct(double v) { return printf1("%.1f dB/oct", v); }

std::string pct(double v) { return minusFix(printf1("%.0f", std::round(v))) + " %"; }

std::string count(long long n) {
    std::string d = std::to_string(std::llabs(n)), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return (n < 0 ? std::string(kMinus) : std::string()) + out;
}

std::string latency(long long samples) { return count(samples) + " smp"; }

std::string dbAxis(double v, bool signPositive) {
    const double r = std::round(v * 10.0) / 10.0;
    std::string mag = printf1("%g", std::abs(r));
    if (r < 0.0) return std::string(kMinus) + mag;
    if (r > 0.0 && signPositive) return "+" + mag;
    return mag;
}

std::string pan(double v, bool midSide) {
    if (std::abs(v) < 0.05) return "C";
    const char side = midSide ? (v < 0 ? 'M' : 'S') : (v < 0 ? 'L' : 'R');
    return printf1("%.1f", std::abs(v)) + " " + side;
}

}  // namespace amq::ui::fmt
