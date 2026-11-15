#pragma once
// The editor's only route to parameters (area 09 R4, area 10, area 21 R5). Implemented by the
// plugin; every call is on the main (GUI) thread.
//
// performEdit writes the table through set(id, plain, GUI); begin/perform/end are forwarded to
// the host as gesture and value events, so the host records one automation gesture per edit.

#include "amq/analysis/snapshot.hpp"
#include "amq/params/parameter_table.hpp"

namespace amq::ui {

class EditController {
public:
    virtual ~EditController() = default;

    [[nodiscard]] virtual const params::ParameterTable& table() const noexcept = 0;
    virtual void addListener(params::ParameterListener* l) = 0;
    virtual void removeListener(params::ParameterListener* l) = 0;
    // Delivers pending table notifications now (the editor calls it after its own edits).
    virtual void dispatchNotifications() = 0;

    virtual void beginEdit(params::ParamIndex index) = 0;
    virtual void performEdit(params::ParamIndex index, double plain) = 0;
    virtual void endEdit(params::ParamIndex index) = 0;

    // Snapshot feed (areas 11, 12, 14): the newest frame of each type, or nullptr if nothing new
    // since the last call. The pointer stays valid until the next call of the same function.
    virtual const analysis::LevelSnapshot* pullLevels() noexcept { return nullptr; }
    virtual const analysis::GainChangeSnapshot* pullGainChange() noexcept { return nullptr; }
    virtual const analysis::SpectrumSnapshot* pullSpectrum() noexcept { return nullptr; }
};

}  // namespace amq::ui
