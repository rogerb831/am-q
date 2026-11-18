#pragma once
// Am-Q editor (areas 12, 13, 15): a Visage window embedded in the host's parent view
// (docs/plan/decisions.md D1). The plugin creates it on gui.create, attaches it on
// gui.set_parent and destroys it on gui.destroy, all on the main thread.
//
// Content and layout follow docs/design/editor-spec.md (bars, the response graph over the analyzer,
// the floating band panel, menus and popovers, EQ Match mode), with the region map in
// EditorLayout. Graph input goes through GraphInteraction, keys through KeyDispatcher (focus first,
// then host fall-through). Every edit goes through the UndoController (set(id, plain, GUI) with host
// gestures; GUI edits are undoable). The spec, its PROPOSED items included, was approved by the
// product owner on 2026-11-18 (the Legal UI review was marked complete the same day).
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
    // Logical pixels. Default 1280x720 (decided 2026-11-17, spec C-1); limits per spec §1.2 (approved 2026-11-18).
    static constexpr uint32_t kDefaultWidth = 1280;
    static constexpr uint32_t kDefaultHeight = 720;
    static constexpr uint32_t kMinWidth = 640;
    static constexpr uint32_t kMinHeight = 360;
    static constexpr uint32_t kMaxWidth = 3840;
    static constexpr uint32_t kMaxHeight = 2160;

    // CLAP window API for this platform (CLAP_WINDOW_API_WIN32 / _COCOA / _X11).
    static const char* windowApi() noexcept;

    // Interface zoom (spec §1.3, P-7): the user scale on top of the OS DPI. The layout space is
    // the window's logical size divided by the zoom.
    static constexpr double kZooms[] = {1.0, 1.5, 2.0, 3.0};

    // Clamp a host-unit size to the logical limits at the given host-unit scale and zoom: the
    // minimum grows with the zoom (640z x 360z window-logical, at most the maximum). Pure: safe on
    // any thread (gui.adjust_size can come from VST3 size checks).
    static void constrainHostSize(uint32_t* w, uint32_t* h, double scale, double zoom = 1.0) noexcept;

    // logicalWidth/Height: the size to open at (the plugin remembers it across close/reopen).
    // logicalWidth/Height are window-logical pixels (OS DPI only, no zoom).
    explicit Editor(UndoController& controller, uint32_t logicalWidth = kDefaultWidth,
                    uint32_t logicalHeight = kDefaultHeight, double zoom = 1.0);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // Main thread only from here on.
    void attach(void* nativeParent);                 // HWND, NSView* or X11 window
    bool hostSize(uint32_t* w, uint32_t* h) const;  // current size in host units
    void setHostSize(uint32_t w, uint32_t h);       // gui.set_size (already constrained)
    [[nodiscard]] double hostScale() const noexcept;  // host units per window-logical pixel
    [[nodiscard]] float logicalWidth() const noexcept;   // window-logical (OS DPI only)
    [[nodiscard]] float logicalHeight() const noexcept;
    [[nodiscard]] double zoom() const noexcept;
    void setZoom(double zoom);  // keeps the layout size: asks the host for the zoomed window size

    // Called after every layout, with the size in host units and the host-unit scale.
    std::function<void(uint32_t w, uint32_t h, double scale)> onSizeChanged;
    // The DPI changed: the editor asks the host for this host-unit size. Return true if the
    // host accepted the request (it then calls gui.set_size).
    std::function<bool(uint32_t w, uint32_t h)> onRequestResize;
    // The user changed the interface zoom (the plugin saves it with the state).
    std::function<void(double zoom)> onZoomChanged;

    // Root of the accessible-control tree (area 15 R12).
    [[nodiscard]] AccessibleControl& accessibleRoot() noexcept;

    // Live editor count, for open/close soak tests (area 12 R1).
    static int liveCount() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amq::ui
