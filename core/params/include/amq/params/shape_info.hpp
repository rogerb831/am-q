#pragma once
// Static shape descriptors (area 08 R5, area 15 R8): one source for the GUI, EQ Match and any
// future host hint. Stored shape ids are the dsp::Shape values and never change (area 15 R7).

#include "amq/dsp/analog_prototype.hpp"

#include <string_view>

namespace amq::params {

enum class ShapeFamily : uint8_t { BellType, Shelf, LowCut, HighCut, Notch, BandPass, Tilt, AllPass };

struct ShapeInfo {
    dsp::Shape shape;
    std::string_view name;
    ShapeFamily family;
    bool hasGain;
    bool hasQ;
    bool hasSlope;
};

[[nodiscard]] ShapeInfo shapeInfo(dsp::Shape shape) noexcept;

}  // namespace amq::params
