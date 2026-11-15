#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cmath>

namespace amq::params {

double qFromPosition(double n) noexcept {
    n = std::clamp(n, 0.0, 1.0);
    return kQMin * std::pow(kQMax / kQMin, n);
}

double positionFromQ(double q) noexcept {
    q = std::clamp(q, kQMin, kQMax);
    return std::log(q / kQMin) / std::log(kQMax / kQMin);
}

namespace {
const double kFadeGain = std::pow(10.0, kOutputLevelMinDb / 20.0);  // amplitude at -36 dB
}

double outputLevelDbFromPosition(double p) noexcept {
    p = std::clamp(p, 0.0, 1.0);
    if (p >= kOutputFadeStart)
        return kOutputLevelMinDb +
               (kOutputLevelMaxDb - kOutputLevelMinDb) * (p - kOutputFadeStart) / (1.0 - kOutputFadeStart);
    if (p <= 0.0) return kOutputSilenceDb;
    const double r = p / kOutputFadeStart;
    const double amp = kFadeGain * r * r;  // smooth, monotonic fade to silence
    return std::max(20.0 * std::log10(amp), kOutputSilenceDb);
}

double outputLevelPositionFromDb(double db) noexcept {
    if (db >= kOutputLevelMinDb) {
        db = std::min(db, kOutputLevelMaxDb);
        return kOutputFadeStart +
               (1.0 - kOutputFadeStart) * (db - kOutputLevelMinDb) / (kOutputLevelMaxDb - kOutputLevelMinDb);
    }
    if (db <= kOutputSilenceDb) return 0.0;
    const double amp = std::pow(10.0, db / 20.0);
    return kOutputFadeStart * std::sqrt(amp / kFadeGain);
}

double outputLevelGain(double db) noexcept {
    return db <= kOutputSilenceDb ? 0.0 : std::pow(10.0, db / 20.0);
}

}  // namespace amq::params
