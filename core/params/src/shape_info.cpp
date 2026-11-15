#include "amq/params/shape_info.hpp"

namespace amq::params {

namespace {

ShapeFamily familyOf(dsp::Shape s) noexcept {
    switch (s) {
        case dsp::Shape::Bell: return ShapeFamily::BellType;
        case dsp::Shape::LowShelf:
        case dsp::Shape::HighShelf: return ShapeFamily::Shelf;
        case dsp::Shape::LowCut: return ShapeFamily::LowCut;
        case dsp::Shape::HighCut: return ShapeFamily::HighCut;
        case dsp::Shape::Notch: return ShapeFamily::Notch;
        case dsp::Shape::BandPass: return ShapeFamily::BandPass;
        case dsp::Shape::TiltShelf:
        case dsp::Shape::FlatTilt: return ShapeFamily::Tilt;
        case dsp::Shape::AllPass: return ShapeFamily::AllPass;
    }
    return ShapeFamily::BellType;
}

}  // namespace

// The gain, Q and slope flags come from core/dsp, so the DSP and the GUI cannot disagree.
ShapeInfo shapeInfo(dsp::Shape s) noexcept {
    return {s, dsp::shapeName(s), familyOf(s), dsp::shapeHasGain(s), dsp::shapeUsesQ(s), dsp::shapeUsesSlope(s)};
}

}  // namespace amq::params
