#include "amq/ui/key_dispatcher.hpp"

#include "amq/dsp/conventions.hpp"
#include "amq/params/shape_info.hpp"
#include "amq/ui/edit_gesture.hpp"

#include <algorithm>
#include <cmath>

namespace amq::ui {

using params::BandField;

bool KeyDispatcher::nudge(double dOct, double dDb, double dQ) {
    const auto t = targets();
    if (t.empty()) return false;
    auto& c = ops_.controller();
    UndoGroup group(c, "Nudge");
    std::vector<params::ParamIndex> ids;
    for (int b : t) {
        if (dOct != 0.0) ids.push_back(params::bandParam(b, BandField::Freq));
        if (dDb != 0.0) {
            const auto shape = static_cast<dsp::Shape>(static_cast<int>(c.table().value(params::bandParam(b, BandField::Shape))));
            ids.push_back(params::bandParam(b, params::shapeInfo(shape).hasGain ? BandField::Gain : BandField::Q));
        }
    }
    EditGesture gesture;
    gesture.open(c, ids);
    for (int b : t) {
        if (dOct != 0.0) {
            const auto id = params::bandParam(b, BandField::Freq);
            gesture.set(id, std::clamp(c.table().value(id) + dOct, std::log2(dsp::kMinBandFrequencyHz),
                                       std::log2(dsp::kMaxBandFrequencyHz)));
        }
        if (dDb != 0.0) {
            const auto shape = static_cast<dsp::Shape>(static_cast<int>(c.table().value(params::bandParam(b, BandField::Shape))));
            if (params::shapeInfo(shape).hasGain) {
                const auto id = params::bandParam(b, BandField::Gain);
                gesture.set(id, c.table().value(id) + dDb);
            } else {
                const auto id = params::bandParam(b, BandField::Q);
                gesture.set(id, c.table().value(id) + (dDb > 0 ? dQ : -dQ));
            }
        }
    }
    return true;
}

bool KeyDispatcher::keyDown(const KeyEvent& e) {
    const auto& m = e.mods;
    switch (e.key) {
        case Key::Delete:
        case Key::Backspace: {
            const auto t = targets();
            if (t.empty()) return false;
            ops_.remove(t);
            g_.clearSelection();
            return true;
        }
        case Key::Escape:
            if (g_.dragging()) {
                g_.cancelDrag();
                return true;
            }
            if (!g_.selection().empty()) {
                g_.clearSelection();
                return true;
            }
            return false;
        case Key::Left: return nudge(-(m.shift ? kFineOct : kNudgeOct), 0.0, 0.0);
        case Key::Right: return nudge(m.shift ? kFineOct : kNudgeOct, 0.0, 0.0);
        case Key::Up: return nudge(0.0, m.shift ? kFineDb : kNudgeDb, kNudgeQ);
        case Key::Down: return nudge(0.0, -(m.shift ? kFineDb : kNudgeDb), kNudgeQ);
        case Key::Tab:
            if (ops_.usedBands().empty()) return false;
            g_.focusNext(m.shift ? -1 : 1);
            return true;
        case Key::Character: {
            if (!m.command) return false;  // plain letters and Space go to the host
            auto& u = ops_.controller();
            switch (e.ch) {
                case U'z':
                    if (m.shift) return u.redo() || true;
                    return u.undo() || true;
                case U'y': return u.redo() || true;
                case U'a':
                    g_.selectAll();
                    return true;
                case U'c':
                case U'x': {
                    const auto t = targets();
                    if (t.empty()) return false;
                    clipboard = e.ch == U'c' ? ops_.copy(t) : ops_.cut(t);
                    if (e.ch == U'x') g_.clearSelection();
                    if (writeSystemClipboard) writeSystemClipboard(clipboard);
                    return true;
                }
                case U'v': {
                    if (readSystemClipboard) {
                        const std::string s = readSystemClipboard();
                        if (s.find("\"am-q.bands\"") != std::string::npos) clipboard = s;
                    }
                    BandOps::Bands made;
                    if (!ops_.paste(clipboard, {}, &message, &made)) return true;  // handled: the message shows
                    message.clear();
                    g_.setSelection(made, made.empty() ? -1 : made.front());
                    return true;
                }
                default: return false;
            }
        }
        default: return false;
    }
}

}  // namespace amq::ui
