#pragma once
// Processor-to-GUI snapshot types (areas 11 R3-R6, 12 R10/R11, 14 R12-R14).
//
// Every snapshot carries a header: an increasing stamp (0 = nothing published yet) and the
// sample position of its end. A default-constructed snapshot is the silence floor, so the
// editor never shows garbage before the first publish (area 12 R11).

#include <array>
#include <cstdint>

namespace amq::analysis {

inline constexpr float kFloorDb = -150.0f;  // PROVISIONAL display floor for meters and spectra
inline constexpr uint32_t kMaxChannels = 2;
inline constexpr int kBandCount = 24;           // = params::kBandCount (checked in the engine)
inline constexpr uint32_t kMaxSpectrumBins = 2048;  // PROVISIONAL until the analyzer (area 19)

struct SnapshotHeader {
    uint64_t stamp = 0;           // 1, 2, 3, ... per publish of this type
    uint64_t samplePosition = 0;  // engine sample position at the end of the frame
};

// Per channel. peakDb is the largest sample magnitude since the previous frame the GUI read
// (peaks of dropped frames are max-merged in, area 11 R4). rmsDb is the smoothed RMS now.
// holdDb is the peak hold.
struct ChannelLevel {
    float peakDb = kFloorDb;
    float rmsDb = kFloorDb;
    float holdDb = kFloorDb;
};

struct LevelSnapshot {
    SnapshotHeader header;
    uint32_t channels = 0;  // channels metered (1 or 2)
    std::array<ChannelLevel, kMaxChannels> input{};
    std::array<ChannelLevel, kMaxChannels> output{};
};

// Per band: the current signed dynamic gain change in dB, and the change with the largest
// magnitude since the previous frame the GUI read (area 11 R5, area 14 R12). 0 dB until the
// dynamics arrive (area 06).
struct GainChangeSnapshot {
    SnapshotHeader header;
    std::array<float, kBandCount> currentDb{};
    std::array<float, kBandCount> peakDb{};
};

// Spectra in dB per bin (area 14 R13). Bin layout comes with the analyzer (area 19).
struct SpectrumSnapshot {
    SnapshotHeader header;
    uint32_t binCount = 0;
    std::array<float, kMaxSpectrumBins> pre;
    std::array<float, kMaxSpectrumBins> post;
    std::array<float, kMaxSpectrumBins> external;
    SpectrumSnapshot() {
        pre.fill(kFloorDb);
        post.fill(kFloorDb);
        external.fill(kFloorDb);
    }
};

}  // namespace amq::analysis
