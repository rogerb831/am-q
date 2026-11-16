#pragma once
// EQ Match apply (area 16 R11-R14). Writes a fit into bands through the setter with source EQ Match
// and keeps the whole apply as one undo step: the first apply after a new fit opens an undo group,
// and every Amount or Bands slider move merges into it (R13): while a slider moves the group is open,
// and a later move reopens the same step as long as it is still the latest one (nothing else was
// edited in between). finish() closes it (slider released, panel closed).
//
//   Bands slider: the first `count` fitted bands (strongest first, R10).
//   Amount (0..1): scales every fitted gain in dB (R11); for cuts it moves the slope towards the
//     gentlest (6 dB/oct), and below kCutBypassAmount the cut is left out (PROVISIONAL, area 16 open
//     question).
//   Slots: bands an earlier match created (tagged) are reused first, so a re-apply replaces them
//     rather than adding more; then free bands. Each written band starts from every field's default
//     (like band creation), then Used, Enabled, shape, slope, frequency, gain and Q. Tagged bands no
//     longer needed (a smaller count) are deleted. If the free bands are too few, as many as fit are
//     applied, strongest first, and the message says how many were dropped (R14).

#include "amq/ui/match_host.hpp"
#include "amq/ui/undo_controller.hpp"

#include <string>
#include <vector>

namespace amq::ui {

inline constexpr double kCutBypassAmount = 0.1;  // PROVISIONAL

class EqMatchApplier {
public:
    explicit EqMatchApplier(UndoController& c) : c_(c) {}
    ~EqMatchApplier() { finish(); }

    void setFit(std::vector<MatchBand> bands);  // a new fit; closes any open apply
    [[nodiscard]] const std::vector<MatchBand>& fit() const noexcept { return fit_; }
    // Applies; returns the number of bands written. message: set when bands were dropped (R14).
    int apply(double amount, int count, std::string* message);
    void finish();  // closes the undo step (slider released, panel closed)
    [[nodiscard]] const std::vector<int>& taggedBands() const noexcept { return tagged_; }
    // The applied version of a fitted band at an amount (also what the overlay draws).
    [[nodiscard]] static MatchBand scaled(const MatchBand& b, double amount);

private:
    UndoController& c_;
    std::vector<MatchBand> fit_;
    std::vector<int> tagged_;
    bool open_ = false;
    bool applied_ = false;  // this fit has an EQ Match step that later moves may amend
};

}  // namespace amq::ui
