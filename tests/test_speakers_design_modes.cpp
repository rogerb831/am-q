// Speaker model (area 01 R33, area 02 R7) and design-mode selection (area 02 R15).
#include "amq/dsp/helper_filters.hpp"
#include "amq/dsp/speakers.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <complex>

using namespace amq::dsp;

TEST_CASE("Speaker groups on the v1 layouts") {
    // Stereo L/R at -30 / +30 degrees: front, ear level, a mirrored pair.
    CHECK(speakerMask(SpeakerGroup::All, ChannelLayout::Stereo) == 3);
    CHECK(speakerMask(SpeakerGroup::NonLfe, ChannelLayout::Stereo) == 3);
    CHECK(speakerMask(SpeakerGroup::Front, ChannelLayout::Stereo) == 3);
    CHECK(speakerMask(SpeakerGroup::AzimuthPairs, ChannelLayout::Stereo) == 3);
    CHECK(speakerMask(SpeakerGroup::Height, ChannelLayout::Stereo) == 0);
    CHECK(speakerMask(SpeakerGroup::Lfe, ChannelLayout::Stereo) == 0);
    CHECK(speakerMask(SpeakerGroup::Centre, ChannelLayout::Stereo) == 0);
    CHECK(speakerMask(SpeakerGroup::RearCentre, ChannelLayout::Stereo) == 0);
    // Mono: one front centre speaker.
    CHECK(speakerMask(SpeakerGroup::All, ChannelLayout::Mono) == 1);
    CHECK(speakerMask(SpeakerGroup::Centre, ChannelLayout::Mono) == 1);
    CHECK(speakerMask(SpeakerGroup::Front, ChannelLayout::Mono) == 1);
    CHECK(speakerMask(SpeakerGroup::AzimuthPairs, ChannelLayout::Mono) == 0);
    CHECK(speakerMask(SpeakerGroup::Lfe, ChannelLayout::Mono) == 0);
}

TEST_CASE("Speaker groups on surround descriptors (model only in v1)") {
    const SpeakerDescriptor c{0, false, false}, ls{-110, false, false}, lfe{0, true, false}, top{45, false, true},
        rc{180, false, false};
    CHECK(speakerGroupMatches(SpeakerGroup::NonLfe, c));
    CHECK_FALSE(speakerGroupMatches(SpeakerGroup::NonLfe, lfe));
    CHECK(speakerGroupMatches(SpeakerGroup::Lfe, lfe));
    CHECK_FALSE(speakerGroupMatches(SpeakerGroup::Centre, lfe));
    CHECK(speakerGroupMatches(SpeakerGroup::Height, top));
    CHECK_FALSE(speakerGroupMatches(SpeakerGroup::Front, top));
    CHECK_FALSE(speakerGroupMatches(SpeakerGroup::Front, ls));
    CHECK(speakerGroupMatches(SpeakerGroup::AzimuthPairs, ls));
    CHECK(speakerGroupMatches(SpeakerGroup::RearCentre, rc));
    CHECK_FALSE(speakerGroupMatches(SpeakerGroup::AzimuthPairs, rc));
    // BS.1770 channel weights for Auto Gain.
    CHECK(speakerLoudnessWeight(c) == 1.0);
    CHECK(speakerLoudnessWeight(ls) == 1.41);
    CHECK(speakerLoudnessWeight(lfe) == 0.0);
}

TEST_CASE("Design modes: user bands matched, All Pass bilinear at the centre, helpers bandwidth pre-warped") {
    for (int s = 0; s < kShapeCount; ++s) {
        const auto shape = static_cast<Shape>(s);
        CHECK(designModeFor(shape, FilterRole::UserBand) ==
              (shape == Shape::AllPass ? DesignMode::BilinearCentre : DesignMode::MagnitudeMatched));
        CHECK(designModeFor(shape, FilterRole::Helper) == DesignMode::BilinearBandwidth);
    }
}

TEST_CASE("Helper low-pass and high-pass: -3 dB exactly at the cutoff") {
    constexpr double kPi = 3.14159265358979323846;
    for (double fs : {44100.0, 96000.0})
        for (double hz : {30.0, 1000.0, 15000.0}) {
            const auto z = std::polar(1.0, 2.0 * kPi * hz / fs);
            CHECK(std::abs(svfResponse(helperLowPass(hz, fs), z)) == doctest::Approx(std::sqrt(0.5)).epsilon(1e-12));
            CHECK(std::abs(svfResponse(helperHighPass(hz, fs), z)) == doctest::Approx(std::sqrt(0.5)).epsilon(1e-12));
            CHECK(std::abs(svfResponse(helperBandPass(hz, 2.0, fs), z)) == doctest::Approx(1.0).epsilon(1e-12));
        }
}
