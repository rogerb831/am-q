#pragma once
// Native open-file dialog for the EQ Match file reference (area 17 R2). Returns a UTF-8 path, or an
// empty string when cancelled or when the platform has no dialog yet (macOS: with the first macOS
// build). Main thread only; modal.

#include <filesystem>
#include <string>

namespace amq::ui {

std::string pickAudioFile();

// Native save dialog for a preset (ui/presets.hpp): starts in folder (created if missing) with the
// suggested name; returns a UTF-8 path ending in .amqpreset, or "" when cancelled.
std::string pickPresetSavePath(const std::filesystem::path& folder, const std::string& suggestedName);

}  // namespace amq::ui
