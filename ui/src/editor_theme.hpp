#pragma once
// Tokens, fonts, icons and drawing primitives for the editor (docs/design/editor-spec.md §0;
// docs/design/reference/ds/am-q/tokens.json). Internal to amq_ui.

#include "amq/ui/editor_layout.hpp"

#include <visage/graphics.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "embedded/fonts.h"

namespace amq::ui::theme {

// Colour tokens, ARGB.
namespace tok {
constexpr uint32_t surface000 = 0xff0d0d10, surface100 = 0xff17161a, surface200 = 0xff201f24, surface300 = 0xff2b2a30,
                   surface400 = 0xff37363d;
constexpr uint32_t lineGrid = 0xff26252b, lineHairline = 0xff3d3c43, lineZero = 0xff8f8d96;
constexpr uint32_t ink = 0xffeceaf0, inkMuted = 0xffa5a3ad, inkAxis = 0xff8c8a93, inkDisabled = 0xff5e5c66;
constexpr uint32_t accent = 0xffffb81c, accentHover = 0xffffc94d, accentInk = 0xff1a1305;
constexpr uint32_t analyzerPre = 0xff3a3940, analyzerPost = 0xff7c7a84, analyzerSc = 0xff5b6e8a;
constexpr uint32_t meterOk = 0xff3faf3a, meterHot = 0xffd9d33a, signalClip = 0xffff3b30, signalSolo = 0xff2fe0b0;
constexpr uint32_t band[8] = {0xffc4ee3c, 0xffff4d4d, 0xff3aa0ff, 0xffd84cf2, 0xff7d62ff, 0xff2fe0b0, 0xffff8a3d, 0xffff63ae};
constexpr float opacityBandFill = 0.38f, opacityBandSelected = 0.55f, opacityBandIdle = 0.18f, opacityBypassed = 0.35f,
                opacityPreview = 0.6f;
}  // namespace tok

inline uint32_t bandColour(int band) { return tok::band[static_cast<size_t>(band % 8)]; }
inline uint32_t withAlpha(uint32_t argb, float a) {
    const auto al = static_cast<uint32_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * static_cast<float>(argb >> 24)));
    return (argb & 0x00ffffffu) | (al << 24);
}

// Fonts: Inter is the spec's family; until it is bundled (a licence and download decision), the
// spec's first fallback, Segoe UI, is read from the system on Windows; elsewhere the embedded Lato.
struct Fonts {
    visage::Font label13, semi13, semi12, caption12, scale12, micro10, preset15, wordmark30;
    Fonts();
};
const Fonts& fonts();
inline float textWidth(const visage::Font& f, const std::string& s) { return f.stringWidth(visage::String(s).toUtf32()); }

// Icons: SVG path data in a 16-unit box (spec §0.3), parsed once.
const visage::Path& iconPath(const std::string& svg);
void icon(visage::Canvas& c, const std::string& svg, float x, float y, uint32_t colour, float stroke = 1.5f);
void glyphCircle(visage::Canvas& c, float cx, float cy, float r, uint32_t colour, float stroke = 1.5f);

namespace icons {
inline const std::string undo = "M5 3 L2 6 L5 9 M2 6 H10 A4 4 0 0 1 10 14 H7";
inline const std::string redo = "M11 3 L14 6 L11 9 M14 6 H6 A4 4 0 0 0 6 14 H9";
inline const std::string copyAB = "M3 8 H12 M9 5 L12 8 L9 11";
inline const std::string prev = "M10 3 L5 8 L10 13";
inline const std::string next = "M6 3 L11 8 L6 13";
inline const std::string matchA = "M1 10 C4 10 4 5 7 5 C10 5 10 11 15 8";
inline const std::string matchB = "M1 12 C4 12 5 8 8 8 C11 8 12 10 15 10";
inline const std::string gear = "M8 1.5 V3.5 M8 12.5 V14.5 M1.5 8 H3.5 M12.5 8 H14.5 M3.4 3.4 L4.8 4.8 M11.2 11.2 L12.6 12.6 M3.4 12.6 L4.8 11.2 M11.2 4.8 L12.6 3.4";
inline const std::string fullscreen = "M2 6 V2 H6 M10 2 H14 V6 M14 10 V14 H10 M6 14 H2 V10";
inline const std::string help = "M6.5 6.2 A1.6 1.6 0 1 1 8.6 7.7 C8 8 8 8.6 8 9.2 M8 11.5 V11.6";
inline const std::string power = "M8 2 V8 M4.5 4.5 A5 5 0 1 0 11.5 4.5";
inline const std::string headphones = "M3 10 V8 A5 5 0 0 1 13 8 V10 M2 11 H5 V14 H2 Z M11 11 H14 V14 H11 Z";
inline const std::string close = "M4 4 L12 12 M12 4 L4 12";
inline const std::string freeze = "M8 2 V14 M2.8 5 L13.2 11 M2.8 11 L13.2 5";
inline const std::string invertSlash = "M12.5 3.5 L3.5 12.5";
inline const std::string check = "M3 8.5 L6.5 12 L13 4.5";
inline const std::string dash = "M4 8 H12";
inline const std::string invertGain = "M2 8 C5 8 5 3 8 3 M8 13 C11 13 11 8 14 8";
inline const std::string makeDynamic = "M8 3 V13 M5 6 L8 3 L11 6";
inline const std::string makeSpectral = "M2 12 V9 M5 12 V6 M8 12 V8 M11 12 V4 M14 12 V7";
inline const std::string slope = "M2 4 L14 12";
inline const std::string copy = "M5 5 H13 V14 H5 Z M3 11 V3 H10";
inline const std::string paste = "M4 3 H12 V14 H4 Z";
inline const std::string scissors = "M5.5 10.5 L12 2 M10.5 10.5 L4 2";
inline const std::string split = "M8 2 V14 M3 6 L8 2 L13 6";
inline const std::string chevDown = "M3 4.5 L6 7.5 L9 4.5";
inline const std::string chevUp = "M3 7.5 L6 4.5 L9 7.5";
inline const std::string chevRight = "M4.5 3 L7.5 6 L4.5 9";
inline const std::string overflow = "M3.5 8 H3.6 M8 8 H8.1 M12.5 8 H12.6";
inline const std::string record = "";  // drawn as a filled circle
inline const std::string reset = "M3 8 A5 5 0 1 0 5 4 M5 1.5 V4.5 H8";
inline const std::string warning = "M8 2 L14.5 13.5 H1.5 Z M8 6.5 V9.5 M8 11.5 V11.6";
std::string shapeGlyph(int shape);  // spec §0.3 shape glyphs, by dsp::Shape id
}  // namespace icons

// Primitives (spec §0.2). States per panels §3.1.
struct ControlState {
    bool hover = false, pressed = false, on = false, enabled = true, open = false;
};
void panel(visage::Canvas& c, const Rect& r, float radius = 16.0f);
void iconButton(visage::Canvas& c, const Rect& r, const std::string& svg, ControlState s, bool muted = false);
void chip(visage::Canvas& c, const Rect& r, const std::string& text, ControlState s);
// A select: optional leading glyph, text, optional muted text, chevron (0 none, 1 down, -1 up).
void select(visage::Canvas& c, const Rect& r, const std::string& glyph, const std::string& text,
            const std::string& muted, int chevron, ControlState s, bool mutedFirst = false);
float selectWidth(const std::string& glyph, const std::string& text, const std::string& muted, int chevron);
float chipWidth(const std::string& text);
void segmented(visage::Canvas& c, const Rect& r, const std::vector<std::string>& labels, int on, int hover, bool enabled = true);
void readout(visage::Canvas& c, const Rect& r, const std::string& text, bool accentVariant, bool hover = false,
             uint32_t dot = 0, bool ring = false);
float readoutWidth(const std::string& text, bool dot = false);
void text(visage::Canvas& c, const visage::Font& f, const std::string& s, float x, float y, float w, float h, uint32_t colour,
          visage::Font::Justification j = visage::Font::kLeft);
void separator(visage::Canvas& c, float x, float cy);
// A knob (spec panels §2.3 "Knob drawing"). n in 0..1; bipolar arcs start at the top.
struct KnobStyle {
    bool bipolar = false, greyed = false, hover = false, dragging = false;
    float grFraction = 0.0f;  // inner gain-change arc (-1..1 of the half sweep), 0 = none
    uint32_t grColour = 0;
};
void knob(visage::Canvas& c, float x, float y, float size, double n, const std::string& label, const std::string& value,
          KnobStyle st);
void dashedPath(visage::Canvas& c, const visage::Path& p, float w, float h, float thickness, std::vector<float> dash);

// Polylines drawn with a tight bounding box: Visage gives every path fill or stroke an atlas slot
// the size of its box, so full-frame boxes for many curves overflow the atlas.
class Poly {
public:
    void moveTo(float x, float y) { subs_.push_back({{x, y}}); }
    void lineTo(float x, float y) {
        if (subs_.empty()) subs_.push_back({});
        subs_.back().push_back({x, y});
    }
    void close() { closed_ = true; }
    void addRectangle(float x, float y, float w, float h) {
        moveTo(x, y), lineTo(x + w, y), lineTo(x + w, y + h), lineTo(x, y + h), lineTo(x, y);
    }
    void addCircle(float cx, float cy, float r) {
        for (int i = 0; i <= 48; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / 48.0f;
            if (i == 0) moveTo(cx + r, cy);
            else lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
        }
    }
    [[nodiscard]] bool empty() const { return subs_.empty(); }
    [[nodiscard]] const std::vector<std::vector<std::pair<float, float>>>& subs() const { return subs_; }
    [[nodiscard]] bool closed() const { return closed_; }

private:
    std::vector<std::vector<std::pair<float, float>>> subs_;
    bool closed_ = false;
};
void fillPoly(visage::Canvas& c, const Poly& p);
void strokePoly(visage::Canvas& c, const Poly& p, float thickness, std::vector<float> dash = {});

}  // namespace amq::ui::theme
