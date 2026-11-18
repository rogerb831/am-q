#pragma once
// Presets (docs/design/editor-spec-graph.md §4.10, P-28; area 21 R14). PROVISIONAL design until the
// Product designer's preset spec:
//   - A preset file holds exactly the plugin's state chunk (the bytes a host saves: AMQS header,
//     parameter pairs, editor trailer), with the extension .amqpreset. Loading one goes through the
//     same batch state load as a host recall, so it is one batch with source Preset and clears the
//     undo history (area 21 R14).
//   - User presets live in one folder per user (presetFolder()); subfolders are categories. A
//     built-in "Init" (every parameter at its default) is always first.
//   - The editor shows the current preset's name and "(edited)" after any change since it was
//     loaded or saved. Previous and Next step through the list in its order: Init, then the
//     presets sorted by category and name, case-insensitively.
// Framework-free: file system only (std::filesystem). The plugin implements PresetHost.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace amq::ui {

struct PresetEntry {
    std::string name;      // file stem, or "Init"
    std::string category;  // subfolder relative to the root, "" for the top level
    std::filesystem::path path;  // empty for Init
    [[nodiscard]] bool builtIn() const { return path.empty(); }
};

inline constexpr const char* kPresetExtension = ".amqpreset";
inline constexpr const char* kInitPresetName = "Init";

// The per-user preset folder: %APPDATA%\Am-Q\Presets on Windows, ~/Library/Audio/Presets/Am-Q on
// macOS, $XDG_CONFIG_HOME/Am-Q/Presets (or ~/.config) elsewhere. Not created here.
[[nodiscard]] std::filesystem::path presetFolder();

// Init first, then every .amqpreset under root (recursively), sorted by category then name.
[[nodiscard]] std::vector<PresetEntry> scanPresets(const std::filesystem::path& root);

// The entry after (direction > 0) or before the current one, wrapping. current < 0: the first or
// last. Returns -1 for an empty list.
[[nodiscard]] int stepPreset(int count, int current, int direction) noexcept;

// The preset menu's search (editor-spec-graph §4.10): the indices into list whose name or category
// contains every space-separated word of query, ignoring ASCII case, in list order. An empty query
// matches everything.
[[nodiscard]] std::vector<int> filterPresets(const std::vector<PresetEntry>& list, const std::string& query);

// A preset name that is safe as a file name: trims, replaces \ / : * ? " < > | and control
// characters with '-', and falls back to "Preset" when nothing is left.
[[nodiscard]] std::string sanitizePresetName(const std::string& name);

bool readFileBytes(const std::filesystem::path& p, std::vector<uint8_t>& out, std::string* error);
bool writeFileBytes(const std::filesystem::path& p, const std::vector<uint8_t>& bytes, std::string* error);

// The editor's view of presets; the plugin implements it over its state save and load.
class PresetHost {
public:
    virtual ~PresetHost() = default;
    // Loads a preset (Init: every parameter to its default) as one batch; false with a message on error.
    virtual bool loadPreset(const PresetEntry& e, std::string* error) = 0;
    // Saves the current state to path (creating folders); the current preset becomes that file.
    virtual bool savePreset(const std::filesystem::path& path, std::string* error) = 0;
    [[nodiscard]] virtual std::string currentName() const = 0;
    [[nodiscard]] virtual std::filesystem::path currentPath() const = 0;
    [[nodiscard]] virtual bool edited() const = 0;
    [[nodiscard]] virtual std::filesystem::path folder() const { return presetFolder(); }
};

}  // namespace amq::ui
