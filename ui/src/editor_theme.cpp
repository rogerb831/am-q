#include "editor_theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace amq::ui::theme {

namespace {

visage::Font loadFont(float size, const char* windowsFile) {
#if defined(_WIN32)
    std::string root = "C:\\Windows";
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "WINDIR") == 0 && env) root = env;
    std::free(env);
    const std::filesystem::path p = std::filesystem::path(root) / "Fonts" / windowsFile;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return visage::Font(size, p.string());
#else
    (void)windowsFile;
#endif
    return visage::Font(size, visage::fonts::Lato_Regular_ttf);
}

}  // namespace

Fonts::Fonts()
    : label13(loadFont(13.0f, "segoeui.ttf")),
      semi13(loadFont(13.0f, "seguisb.ttf")),
      semi12(loadFont(12.0f, "seguisb.ttf")),
      caption12(loadFont(12.0f, "segoeui.ttf")),
      scale12(loadFont(12.0f, "segoeui.ttf")),
      micro10(loadFont(10.0f, "seguisb.ttf")),
      preset15(loadFont(15.0f, "seguisb.ttf")),
      wordmark30(loadFont(30.0f, "segoeuib.ttf")) {}

const Fonts& fonts() {
    static const Fonts f;
    return f;
}

const visage::Path& iconPath(const std::string& svg) {
    static std::map<std::string, visage::Path> cache;
    auto it = cache.find(svg);
    if (it == cache.end()) {
        visage::Path p;
        p.loadSvgPath(svg);
        it = cache.emplace(svg, std::move(p)).first;
    }
    return it->second;
}

void icon(visage::Canvas& c, const std::string& svg, float x, float y, uint32_t colour, float stroke) {
    if (svg.empty()) return;
    c.setColor(colour);
    c.stroke(iconPath(svg), x, y, 16.0f, 16.0f, stroke);
}

void glyphCircle(visage::Canvas& c, float cx, float cy, float r, uint32_t colour, float stroke) {
    c.setColor(colour);
    c.ring(cx - r - stroke * 0.5f, cy - r - stroke * 0.5f, 2.0f * r + stroke, stroke);
}

std::string icons::shapeGlyph(int shape) {
    static const char* g[] = {
        "M1 12 C5 12 6 4 8 4 C10 4 11 12 15 12",                  // Bell
        "M1 5 L4 5 C7 5 7 11 10 11 L15 11",                        // Low Shelf
        "M1 14 C4 14 5 4 9 4 L15 4",                               // Low Cut
        "M1 11 L6 11 C9 11 9 5 12 5 L15 5",                        // High Shelf
        "M1 4 L7 4 C11 4 12 14 15 14",                             // High Cut
        "M1 6 L6 6 C7 6 7.5 14 8 14 C8.5 14 9 6 10 6 L15 6",       // Notch
        "M1 14 C5 14 6 4 8 4 C10 4 11 14 15 14",                   // Band Pass
        "M1 11 L4 11 C7 11 9 5 12 5 L15 5",                        // Tilt Shelf
        "M1 12 L15 4",                                             // Flat Tilt
        "M1 8 L15 8 M8 6 A2 2 0 1 0 8 10 A2 2 0 1 0 8 6",           // All Pass
    };
    return g[std::clamp(shape, 0, 9)];
}

void panel(visage::Canvas& c, const Rect& r, float radius) {
    c.setColor(0x8c000000);  // shadow-panel: 0 16 40 rgba(0,0,0,.55)
    c.roundedRectangleShadow(r.x, r.y + 16.0f, r.w, r.h, radius, 40.0f);
    c.setColor(tok::surface200);
    c.roundedRectangle(r.x, r.y, r.w, r.h, radius);
    c.setColor(tok::lineHairline);
    c.roundedRectangleBorder(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, radius + 1.0f, 1.0f);
}

void iconButton(visage::Canvas& c, const Rect& r, const std::string& svg, ControlState s, bool muted) {
    if (s.on || s.hover || s.pressed) {
        c.setColor(s.pressed ? tok::surface400 : tok::surface300);
        c.roundedRectangle(r.x, r.y, r.w, r.h, 8.0f);
    }
    const uint32_t col = !s.enabled ? tok::inkDisabled : s.on ? tok::accent : muted ? tok::inkMuted : tok::ink;
    icon(c, svg, r.x + (r.w - 16.0f) * 0.5f, r.y + (r.h - 16.0f) * 0.5f, col);
}

float chipWidth(const std::string& t) { return textWidth(fonts().semi13, t) + 20.0f; }

void chip(visage::Canvas& c, const Rect& r, const std::string& t, ControlState s) {
    uint32_t bg = tok::surface300, fg = tok::ink;
    if (!s.enabled) fg = tok::inkDisabled;
    else if (s.on) bg = s.hover || s.pressed ? tok::accentHover : tok::accent, fg = tok::accentInk;
    else if (s.hover || s.pressed || s.open) bg = tok::surface400;
    c.setColor(bg);
    c.roundedRectangle(r.x, r.y, r.w, r.h, 8.0f);
    text(c, fonts().semi13, t, r.x, r.y, r.w, r.h, fg, visage::Font::kCenter);
}

float selectWidth(const std::string& glyph, const std::string& t, const std::string& muted, int chevron) {
    float w = 24.0f + textWidth(fonts().label13, t);
    if (!glyph.empty()) w += 24.0f;
    if (!muted.empty()) w += 8.0f + textWidth(fonts().label13, muted);
    if (chevron) w += 20.0f;
    return w;
}

void select(visage::Canvas& c, const Rect& r, const std::string& glyph, const std::string& t, const std::string& muted,
            int chevron, ControlState s, bool mutedFirst) {
    c.setColor(s.hover || s.open || s.pressed ? tok::surface400 : tok::surface300);
    c.roundedRectangle(r.x, r.y, r.w, r.h, 8.0f);
    const uint32_t fg = s.enabled ? tok::ink : tok::inkDisabled;
    float x = r.x + 12.0f;
    if (!glyph.empty()) {
        icon(c, glyph, x, r.y + (r.h - 16.0f) * 0.5f, fg);
        x += 24.0f;
    }
    const auto& f = fonts().label13;
    auto put = [&](const std::string& s2, uint32_t col) {
        const float w = textWidth(f, s2);
        text(c, f, s2, x, r.y, w + 2.0f, r.h, col);
        x += w + 8.0f;
    };
    if (mutedFirst && !muted.empty()) put(muted, s.enabled ? tok::inkMuted : tok::inkDisabled);
    put(t, fg);
    if (!mutedFirst && !muted.empty()) put(muted, s.enabled ? tok::inkMuted : tok::inkDisabled);
    if (chevron) {
        c.setColor(tok::inkMuted);
        const bool up = (chevron < 0) != s.open;
        c.stroke(iconPath(up ? icons::chevUp : icons::chevDown), r.right() - 24.0f, r.y + (r.h - 12.0f) * 0.5f, 12.0f, 12.0f, 1.5f);
    }
}

void segmented(visage::Canvas& c, const Rect& r, const std::vector<std::string>& labels, int on, int hover, bool enabled) {
    c.setColor(tok::surface000);
    c.roundedRectangle(r.x, r.y, r.w, r.h, 8.0f);
    const auto n = static_cast<float>(std::max<size_t>(labels.size(), 1));
    const float w = (r.w - 4.0f - 2.0f * (n - 1.0f)) / n;
    for (size_t i = 0; i < labels.size(); ++i) {
        const float x = r.x + 2.0f + static_cast<float>(i) * (w + 2.0f);
        const bool sel = static_cast<int>(i) == on;
        if (sel) {
            c.setColor(tok::surface400);
            c.roundedRectangle(x, r.y + 2.0f, w, r.h - 4.0f, 6.0f);
        }
        const uint32_t fg = !enabled ? tok::inkDisabled : (sel || static_cast<int>(i) == hover) ? tok::ink : tok::inkMuted;
        text(c, fonts().semi12, labels[i], x, r.y, w, r.h, fg, visage::Font::kCenter);
    }
}

float readoutWidth(const std::string& t, bool dot) { return textWidth(fonts().semi13, t) + 16.0f + (dot ? 16.0f : 0.0f); }

void readout(visage::Canvas& c, const Rect& r, const std::string& t, bool accentVariant, bool hover, uint32_t dot, bool ring) {
    if (accentVariant || ring) {
        c.setColor(hover ? tok::accentHover : tok::lineHairline);
        c.roundedRectangle(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, 5.0f);
    }
    c.setColor(accentVariant ? tok::surface200 : tok::surface300);
    c.roundedRectangle(r.x, r.y, r.w, r.h, 4.0f);
    float x = r.x + 8.0f;
    if (dot) {
        c.setColor(dot);
        c.circle(x, r.y + (r.h - 8.0f) * 0.5f, 8.0f);
        x += 16.0f;
    }
    text(c, fonts().semi13, t, x, r.y, r.right() - x, r.h, accentVariant ? (hover ? tok::accentHover : tok::accent) : tok::ink);
}

void text(visage::Canvas& c, const visage::Font& f, const std::string& s, float x, float y, float w, float h, uint32_t colour,
          visage::Font::Justification j) {
    if (s.empty()) return;
    c.setColor(colour);
    c.text(visage::String(s), f, j, x, y, w, h);
}

void separator(visage::Canvas& c, float x, float cy) {
    c.setColor(tok::lineHairline);
    c.fill(x, cy - 10.0f, 1.0f, 20.0f);
}

namespace {
constexpr float kPi = 3.14159265f;
float rad(float deg) { return deg * kPi / 180.0f; }
}  // namespace

void knob(visage::Canvas& c, float x, float y, float size, double n, const std::string& label, const std::string& value,
          KnobStyle st) {
    const float cx = x + size * 0.5f, cy = y + size * 0.5f;
    const float sw = size > 70.0f ? 4.0f : 3.0f;
    const float r = size * 0.5f - sw * 0.5f - 1.0f, fr = r - sw - 3.0f;
    // Face with the outer part of shadow-knob.
    c.setColor(0x80000000);
    c.roundedRectangleShadow(cx - fr, cy - fr + 4.0f, 2.0f * fr, 2.0f * fr, fr, 10.0f);
    c.setColor(st.hover ? tok::surface400 : tok::surface300);
    c.circle(cx - fr, cy - fr, 2.0f * fr);
    // Track, value arc and inner gain-change arc. Angles in degrees, 0 at the top, clockwise.
    auto arc = [&](float rr, float d0, float d1, float thick, uint32_t col) {
        if (std::abs(d1 - d0) < 0.5f) return;
        visage::Path p;
        const int steps = std::max(2, static_cast<int>(std::abs(d1 - d0) / 3.0f));
        for (int i = 0; i <= steps; ++i) {
            const float t = rad(d0 + (d1 - d0) * static_cast<float>(i) / static_cast<float>(steps) - 90.0f);
            const float px = cx - x + rr * std::cos(t), py = cy - y + rr * std::sin(t);
            if (i == 0) p.moveTo(px, py);
            else p.lineTo(px, py);
        }
        c.setColor(col);
        c.stroke(p, x, y, size, size, thick, visage::Path::Join::Round, visage::Path::EndCap::Round);
    };
    arc(r, -135.0f, 135.0f, sw, tok::lineHairline);
    const float d = -135.0f + 270.0f * static_cast<float>(std::clamp(n, 0.0, 1.0));
    arc(r, st.bipolar ? 0.0f : -135.0f, d, sw,
        st.greyed ? tok::inkDisabled : (st.hover || st.dragging) ? tok::accentHover : tok::accent);
    if (st.grFraction != 0.0f) arc(r - sw - 1.0f, 0.0f, 135.0f * std::clamp(st.grFraction, -1.0f, 1.0f), 2.0f, st.grColour);
    const float a = rad(d - 90.0f);
    c.setColor(st.greyed ? tok::inkDisabled : tok::ink);
    c.segment(cx + 0.3f * fr * std::cos(a), cy + 0.3f * fr * std::sin(a), cx + (fr - 4.0f) * std::cos(a),
              cy + (fr - 4.0f) * std::sin(a), 2.0f, true);
    std::string up = label;
    for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const float tw = std::max(size, 96.0f);
    text(c, fonts().semi12, up, cx - tw * 0.5f, y + size + 8.0f, tw, 14.0f, st.greyed ? tok::inkMuted : tok::ink,
         visage::Font::kCenter);
    text(c, fonts().semi13, value, cx - tw * 0.5f, y + size + 30.0f, tw, 16.0f,
         st.greyed ? tok::inkDisabled : (st.dragging ? tok::accentHover : tok::accent), visage::Font::kCenter);
}

void dashedPath(visage::Canvas& c, const visage::Path& p, float w, float h, float thickness, std::vector<float> dash) {
    c.stroke(p, 0.0f, 0.0f, w, h, thickness, visage::Path::Join::Round, visage::Path::EndCap::Butt, std::move(dash));
}

namespace {
// Builds a visage::Path in coordinates relative to the poly's padded bounding box.
bool localPath(const Poly& p, float pad, visage::Path& out, Rect& box) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    size_t n = 0;
    for (const auto& s : p.subs())
        for (const auto& [x, y] : s) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y), ++n;
    if (n < 2) return false;
    box = {std::floor(x0 - pad), std::floor(y0 - pad), 0, 0};
    box.w = std::ceil(x1 + pad) - box.x;
    box.h = std::ceil(y1 + pad) - box.y;
    for (const auto& s : p.subs()) {
        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 0) out.moveTo(s[i].first - box.x, s[i].second - box.y);
            else out.lineTo(s[i].first - box.x, s[i].second - box.y);
        }
        if (p.closed()) out.close();
    }
    return true;
}
}  // namespace

void fillPoly(visage::Canvas& c, const Poly& p) {
    visage::Path path;
    Rect b;
    if (localPath(p, 1.0f, path, b)) c.fill(path, b.x, b.y, b.w, b.h);
}

void strokePoly(visage::Canvas& c, const Poly& p, float thickness, std::vector<float> dash) {
    visage::Path path;
    Rect b;
    if (!localPath(p, thickness + 2.0f, path, b)) return;
    c.stroke(path, b.x, b.y, b.w, b.h, thickness, visage::Path::Join::Round,
             dash.empty() ? visage::Path::EndCap::Round : visage::Path::EndCap::Butt, std::move(dash));
}

}  // namespace amq::ui::theme
