#pragma once
// Editor value formats (docs/design/editor-spec-panels.md §2.10). UTF-8, with the Unicode minus
// sign U+2212 and tabular figures assumed. The host-facing formatters in core/params stay as they
// are (area 10); aligning them with these is a follow-up (spec C-29).

#include <string>

namespace amq::ui::fmt {

inline constexpr const char* kMinus = "\xE2\x88\x92";  // U+2212

std::string hz(double hz);                    // 32 Hz · 1.82 kHz · 14.5 kHz
std::string hzAxis(double hz);                // 20 · 1k · 20k
std::string hzRange(double lo, double hi);    // 120–270 Hz · 0.9–3.6 kHz · 6.0–14 kHz
std::string db(double db);                    // +4.5 dB · −3.5 dB · +0.0 dB
std::string dbfs(double db);                  // −20.0 dB
std::string outputDb(double db, double silenceDb);  // fmt-dB, or −∞ dB at the silence floor
std::string q(double q);                      // 0.71 · 4.00 · 12.5
std::string slope(int slopeIndex);            // 24 dB/oct · Brickwall
std::string slopeNumber(int slopeIndex);      // 24 · Brickwall
std::string dbPerOct(double v);               // 4.5 dB/oct
std::string pct(double v);                    // 100 %
std::string count(long long n);               // 1,284
std::string latency(long long samples);       // 4,160 smp
std::string dbAxis(double db, bool signPositive);  // +12 · 0 · −6 (gain) / −10 (analyzer)
std::string pan(double v, bool midSide);      // C · 12.0 L · 30.0 S
std::string minusFix(const std::string& ascii);  // replaces a leading '-' with U+2212

}  // namespace amq::ui::fmt
