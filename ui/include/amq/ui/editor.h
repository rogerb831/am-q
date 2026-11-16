#pragma once
// Am-Q editor (areas 12, 13, 15): a Visage window embedded in the host's parent view
// (docs/plan/decisions.md D1). The plugin creates it on gui.create, attaches it on
// gui.set_parent and destroys it on gui.destroy, all on the main thread.
//
// Content: the response graph (cached ground, the analyzer spectra, the combined curve, band nodes
// with selection, range handles and gain-reduction rings) driven by GraphInteraction; keys through
// KeyDispatcher (focus first, then host fall-through); right-click band and graph menus; and the
// band panel (BandPanel model) with output meters and analyzer toggles. Every edit goes through the
// UndoController (set(id, plain, GUI) with host gestures; GUI edits are undoable). The layout is
// PROVISIONAL until the Product designer's region map and the Legal UI review.
//
// Sizing (area 12 R16). Layout is in logical pixels. The host talks in "host units": physical
// pixels on Windows and Linux, points on macOS (clap/ext/gui.h), so the host-unit scale is the
// monitor DPI scale on Windows and 1 on macOS. Visage is per-monitor DPI aware on Windows, so
// Am-Q declines gui.set_scale and reads the scale from the window. The editor is freely
// resizable between kMin and kMax logical pixels, with no fixed aspect ratio. When the window's
// DPI changes (moved to another monitor, or the Windows scale setting changed), the editor
// keeps its logical size and asks the host to resize it to match.

#include "amq/ui/accessible.h"
#include "amq/ui/undo_controller.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace amq::ui {

class Editor {
public:
    // Logical pixels. Default and limits are PROVISIONAL until the Product designer's region map.
    static constexpr uint32_t kDefaultWidth = 1180;
    static constexpr uint32_t kDefaultHeight = 672;
    static constexpr uint32_t kMinWidth = 640;
    static constexpr uint32_t kMinHeight = 360;
    static constexpr uint32_t kMaxWidth = 3840;
    static constexpr uint32_t kMaxHeight = 2160;

    // CLAP window API for this platform (CLAP_WINDOW_API_WIN32 / _COCOA / _X11).
    static const char* windowApi() noexcept;

    // Clamp a host-unit size to the logical limits at the given host-unit scale. Pure: safe on
    // any thread (gui.adjust_size can come from VST3 size checks).
    static void constrainHostSize(uint32_t* w, uint32_t* h, double scale) noexcept;

    // logicalWidth/Height: the size to open at (the plugin remembers it across close/reopen).
    explicit Editor(UndoController& controller, uint32_t logicalWidth = kDefaultWidth,
                    uint32_t logicalHeight = kDefaultHeight);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // Main thread only from here on.
    void attach(void* nativeParent);                 // HWND, NSView* or X11 window
    bool hostSize(uint32_t* w, uint32_t* h) const;  // current size in host units
    void setHostSize(uint32_t w, uint32_t h);       // gui.set_size (already constrained)
    [[nodiscard]] double hostScale() const noexcept;  // host units per logical pixel
    [[nodiscard]] float logicalWidth() const noexcept;
    [[nodiscard]] float logicalHeight() const noexcept;

    // Called after every layout, with the size in host units and the host-unit scale.
    std::function<void(uint32_t w, uint32_t h, double scale)> onSizeChanged;
    // The DPI changed: the editor asks the host for this host-unit size. Return true if the
    // host accepted the request (it then calls gui.set_size).
    std::function<bool(uint32_t w, uint32_t h)> onRequestResize;

    // Root of the accessible-control tree (area 15 R12).
    [[nodiscard]] AccessibleControl& accessibleRoot() noexcept;

    // Live editor count, for open/close soak tests (area 12 R1).
    static int liveCount() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amq::ui
