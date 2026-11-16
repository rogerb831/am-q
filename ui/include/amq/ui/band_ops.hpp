#pragma once
// Band-operations service (area 14 R4-R10; area 21 R5, R7, R8). The menu, the band panel and the
// keyboard all call these, so every action has one code path. Each call is one undo group and
// writes only through the controller (set(id, plain, GUI) with host gestures).
//
//   create        the first free band, every field at its default, then the gesture's frequency,
//                 gain and shape (area 21 R8: side-chain filter fully open, Spectral Tilt off)
//   remove        Used off and every field back to its default (R6); used by delete, cut and a
//                 paste that replaces bands (R10)
//   setEnabled, invertGain, makeDynamic (a default range when the range is 0, R5), clearDynamics,
//   makeSpectral, clearSpectral, setShape, setSlope (only where the shape allows it), setPlacement
//   copy / paste  the bands as JSON with every field by its string id; paste fills free bands in
//                 order and fails with a message when there are not enough (R10)
//
// Band lists are indices 0..23. Targets (R7): the selection, or the focused band when nothing is
// selected; "all" and "disabled" are the used bands and the used, disabled bands.

#include "amq/dsp/analog_prototype.hpp"
#include "amq/ui/undo_controller.hpp"

#include <string>
#include <vector>

namespace amq::ui {

inline constexpr double kDefaultDynamicRangeDb = -6.0;  // PROVISIONAL (area 14 R5; Product designer)

class BandOps {
public:
    using Bands = std::vector<int>;

    explicit BandOps(UndoController& c) : c_(c) {}

    [[nodiscard]] static Bands targets(const Bands& selection, int focused);
    [[nodiscard]] Bands usedBands() const;
    [[nodiscard]] Bands disabledBands() const;
    [[nodiscard]] int freeBands() const;
    [[nodiscard]] bool used(int b) const;

    int create(double hz, double gainDb, dsp::Shape shape = dsp::Shape::Bell);
    void remove(const Bands& bands);
    void setEnabled(const Bands& bands, bool on);
    void invertGain(const Bands& bands);
    void makeDynamic(const Bands& bands);
    void clearDynamics(const Bands& bands);
    void makeSpectral(const Bands& bands);
    void clearSpectral(const Bands& bands);
    void setShape(const Bands& bands, dsp::Shape shape);
    void setSlope(const Bands& bands, dsp::Slope slope);
    void setPlacement(const Bands& bands, int placement);

    [[nodiscard]] std::string copy(const Bands& bands) const;
    // Pastes into free bands (after removing `replace`, if given). False, with a message, when the
    // clipboard is not ours or the free bands are too few; nothing changes then.
    bool paste(const std::string& json, const Bands& replace, std::string* message, Bands* created = nullptr);
    std::string cut(const Bands& bands);

    [[nodiscard]] UndoController& controller() noexcept { return c_; }
    [[nodiscard]] const params::ParameterTable& table() const noexcept { return c_.table(); }

private:
    double value(int b, params::BandField f) const;
    void write(const std::vector<std::pair<params::ParamIndex, double>>& values);

    UndoController& c_;
};

}  // namespace amq::ui
