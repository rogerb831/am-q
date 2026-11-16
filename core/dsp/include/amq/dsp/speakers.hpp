#pragma once
// Surround-ready speaker model (area 01 R33, area 02 R7). Channels carry a speaker descriptor and
// bands carry a speaker group; a band runs on a channel only when the group matches it (on top of
// its placement). v1 ships mono and stereo only, every band's group is All, and the band field
// (bandNN.speakers) stays reserved and unexposed (area 21 R3, area 14 R11). The model exists now so
// routing, Auto Gain weights and state need no restructuring when surround arrives.

#include <cmath>
#include <cstdint>

namespace amq::dsp {

// One output channel's position. Azimuth in degrees, 0 = front centre, negative = left.
struct SpeakerDescriptor {
    double azimuthDeg = 0.0;
    bool lfe = false;
    bool height = false;
};

enum class SpeakerGroup : uint8_t {
    All = 0,       // every channel
    NonLfe,        // every channel but LFE
    Height,        // height channels
    Front,         // non-LFE ear-level channels within +-60 degrees, centre included
    Lfe,           // LFE only
    Centre,        // ear-level channels at 0 degrees
    AzimuthPairs,  // ear-level channels with a mirror partner (left/right pairs)
    RearCentre,    // ear-level channels at 180 degrees
};
inline constexpr int kSpeakerGroupCount = 8;

enum class ChannelLayout : uint8_t { Mono, Stereo };

// v1 layouts: mono is a front centre speaker; stereo is L/R at -30 / +30 degrees.
[[nodiscard]] inline SpeakerDescriptor speakerOf(ChannelLayout layout, uint32_t channel) noexcept {
    if (layout == ChannelLayout::Mono) return {};
    return {channel == 0 ? -30.0 : 30.0, false, false};
}

[[nodiscard]] inline bool speakerGroupMatches(SpeakerGroup g, const SpeakerDescriptor& s) noexcept {
    const double a = std::abs(s.azimuthDeg);
    switch (g) {
        case SpeakerGroup::All: return true;
        case SpeakerGroup::NonLfe: return !s.lfe;
        case SpeakerGroup::Height: return !s.lfe && s.height;
        case SpeakerGroup::Front: return !s.lfe && !s.height && a <= 60.0;
        case SpeakerGroup::Lfe: return s.lfe;
        case SpeakerGroup::Centre: return !s.lfe && !s.height && a == 0.0;
        case SpeakerGroup::AzimuthPairs: return !s.lfe && !s.height && a > 0.0 && a < 180.0;
        case SpeakerGroup::RearCentre: return !s.lfe && !s.height && a == 180.0;
    }
    return false;
}

// Channel mask (bit c = channel c) of a group on a layout.
[[nodiscard]] inline uint8_t speakerMask(SpeakerGroup g, ChannelLayout layout) noexcept {
    const uint32_t n = layout == ChannelLayout::Mono ? 1u : 2u;
    uint8_t m = 0;
    for (uint32_t c = 0; c < n; ++c)
        if (speakerGroupMatches(g, speakerOf(layout, c))) m = static_cast<uint8_t>(m | (1u << c));
    return m;
}

// Loudness weight of a channel for Auto Gain (area 03 R9, area 05 R12): the ITU-R BS.1770 channel
// weights. LFE is excluded; surrounds beyond +-60 degrees count 1.41 (+1.5 dB); others count 1.
[[nodiscard]] inline double speakerLoudnessWeight(const SpeakerDescriptor& s) noexcept {
    if (s.lfe) return 0.0;
    return std::abs(s.azimuthDeg) > 60.0 && !s.height ? 1.41 : 1.0;
}

}  // namespace amq::dsp
