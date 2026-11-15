#pragma once
// The latency function (area 02 R9-R12, area 05 R2 and R4, area 21 R11 and R13).
//
// A pure function of processing mode, resolution, sample rate and whether a spectral band is
// active. Placement (L/R, M/S or both) and band count are not inputs, so they cannot change it
// (R10). The wrapper caches the value, notifies the host when it changes, and the bypass dry
// delay reads the same value (area 02 design).
//
//   Zero Latency    0, or the FFT-path latency while a spectral band is active.
//   Natural Phase   kernel centre + partition = 256 + 64 = 320 at every rate (area 05 R2).
//   Linear Phase    FIR / 2 + partition (R9). FIR = 4096, 8192, 16384, 32768, 131072 taps for
//                   Low .. Maximum at 44.1 / 48 kHz, times the rate factor (R11); the partition
//                   scales by the same factor (area 05 R4).
//
// The FFT path (R8) is the Linear Phase machinery at the current resolution: area 05 R5 puts
// Linear specs in use "in Linear Phase or while a spectral band is active", and area 07 sizes
// the spectral STFT from the phase-mode resolution. So a spectral band makes the reported
// latency the FFT-path latency in Zero Latency (R9) and, PROVISIONALLY, in Natural Phase too:
// the IIR + kernel path is then delayed to line up with the FFT path, and the larger of the two
// is reported. Area 02 R9 lists Natural Phase as fixed; the spectral case is an open question
// there until area 07 settles the spectral latency.
//
// No oversampling in v1 (R18). An oversampling factor would add its filter delay here.

#include <array>
#include <cstdint>

namespace amq::dsp {

// Order matches the "global.mode" choice parameter.
enum class ProcessingMode : uint8_t { ZeroLatency = 0, NaturalPhase = 1, LinearPhase = 2 };

inline constexpr int kLatencyResolutionCount = 5;  // Low .. Maximum ("global.resolution")
inline constexpr std::array<int, kLatencyResolutionCount> kLinearPhaseBaseTaps = {4096, 8192, 16384, 32768,
                                                                                  131072};  // R11
inline constexpr int kNaturalPhasePartition = 64;      // area 05 R2 (open question: 32?)
inline constexpr int kNaturalPhaseCentre = 256;        // NaturalPhaseKernelBuilder::kCentre
inline constexpr int kLinearPhaseBasePartition = 64;   // PROVISIONAL, at 44.1 / 48 kHz

// Power-of-two rate factor shared by every rate-scaled size (area 02 R11, area 19 R2):
// 1 up to 50 kHz, 2 up to 100 kHz, 4 up to 200 kHz, 8 above.
[[nodiscard]] constexpr int rateFactor(double sampleRate) noexcept {
    if (sampleRate <= 50000.0) return 1;
    if (sampleRate <= 100000.0) return 2;
    if (sampleRate <= 200000.0) return 4;
    return 8;
}

struct LinearPhaseSize {
    int firTaps = 0;
    int partition = 0;
};

// Out-of-range resolutions are clamped.
[[nodiscard]] constexpr LinearPhaseSize linearPhaseSize(int resolution, double sampleRate) noexcept {
    const int r = resolution < 0 ? 0 : (resolution >= kLatencyResolutionCount ? kLatencyResolutionCount - 1 : resolution);
    const int f = rateFactor(sampleRate);
    return {kLinearPhaseBaseTaps[static_cast<size_t>(r)] * f, kLinearPhaseBasePartition * f};
}

// Latency of the FFT path (Linear Phase convolution or the spectral STFT) at this resolution.
[[nodiscard]] constexpr uint32_t fftPathLatency(int resolution, double sampleRate) noexcept {
    const LinearPhaseSize s = linearPhaseSize(resolution, sampleRate);
    return static_cast<uint32_t>(s.firTaps / 2 + s.partition);
}

[[nodiscard]] constexpr uint32_t naturalPhaseLatency() noexcept {
    return static_cast<uint32_t>(kNaturalPhaseCentre + kNaturalPhasePartition);
}

// The latency reported to the host, in samples (R9, R12).
[[nodiscard]] constexpr uint32_t latencySamples(ProcessingMode mode, int resolution, double sampleRate,
                                                bool spectralActive) noexcept {
    const uint32_t fft = fftPathLatency(resolution, sampleRate);
    switch (mode) {
        case ProcessingMode::LinearPhase: return fft;
        case ProcessingMode::NaturalPhase: {
            const uint32_t np = naturalPhaseLatency();
            return spectralActive && fft > np ? fft : np;
        }
        case ProcessingMode::ZeroLatency:
        default: return spectralActive ? fft : 0u;
    }
}

}  // namespace amq::dsp
