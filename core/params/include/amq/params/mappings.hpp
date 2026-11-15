#pragma once
// Pure value mappings used by the parameter table (areas 10, 11).

namespace amq::params {

// Q position n (0..1) <-> Q, Q = Qmin * (Qmax/Qmin)^n with Qmin 0.025, Qmax 40 (area 10 R2).
inline constexpr double kQMin = 0.025;
inline constexpr double kQMax = 40.0;
[[nodiscard]] double qFromPosition(double n) noexcept;
[[nodiscard]] double positionFromQ(double q) noexcept;

// Output Level (area 11 R7): linear in dB from -36 to +36 dB over positions [kFadeStart, 1];
// below kFadeStart the amplitude fades monotonically to silence at position 0.
// PROVISIONAL: kOutputFadeStart and the fade law are the Product designer's to confirm.
inline constexpr double kOutputLevelMinDb = -36.0;
inline constexpr double kOutputLevelMaxDb = 36.0;
inline constexpr double kOutputSilenceDb = -200.0;  // stored floor meaning -inf (silence)
inline constexpr double kOutputFadeStart = 0.1;
[[nodiscard]] double outputLevelDbFromPosition(double position) noexcept;
[[nodiscard]] double outputLevelPositionFromDb(double db) noexcept;
// Linear gain for a stored Output Level, 0 at the silence floor.
[[nodiscard]] double outputLevelGain(double db) noexcept;

}  // namespace amq::params
