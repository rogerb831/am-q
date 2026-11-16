#pragma once
// Native open-file dialog for the EQ Match file reference (area 17 R2). Returns a UTF-8 path, or an
// empty string when cancelled or when the platform has no dialog yet (macOS: with the first macOS
// build). Main thread only; modal.

#include <string>

namespace amq::ui {

std::string pickAudioFile();

}  // namespace amq::ui
