#include "amq/ui/presets.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace amq::ui {

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
#if defined(_WIN32)
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) out = v;
    std::free(v);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

}  // namespace

fs::path presetFolder() {
#if defined(_WIN32)
    const std::string app = env("APPDATA");
    return (app.empty() ? fs::path(env("USERPROFILE")) / "AppData" / "Roaming" : fs::path(app)) / "Am-Q" / "Presets";
#elif defined(__APPLE__)
    return fs::path(env("HOME")) / "Library" / "Audio" / "Presets" / "Am-Q";
#else
    const std::string xdg = env("XDG_CONFIG_HOME");
    return (xdg.empty() ? fs::path(env("HOME")) / ".config" : fs::path(xdg)) / "Am-Q" / "Presets";
#endif
}

std::vector<PresetEntry> scanPresets(const fs::path& root) {
    std::vector<PresetEntry> out;
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || lower(utf8(it->path().extension())) != kPresetExtension) continue;
            PresetEntry e;
            e.name = utf8(it->path().stem());
            const fs::path rel = it->path().parent_path().lexically_relative(root);
            e.category = rel == "." ? std::string() : utf8(rel.generic_u8string());
            e.path = it->path();
            out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) {
        const auto ca = lower(a.category), cb = lower(b.category);
        if (ca != cb) return ca < cb;
        return lower(a.name) < lower(b.name);
    });
    PresetEntry init;
    init.name = kInitPresetName;
    out.insert(out.begin(), init);
    return out;
}

int stepPreset(int count, int current, int direction) noexcept {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) return direction > 0 ? 0 : count - 1;
    return ((current + (direction > 0 ? 1 : -1)) % count + count) % count;
}

std::vector<int> filterPresets(const std::vector<PresetEntry>& list, const std::string& query) {
    std::vector<std::string> words;
    const std::string q = lower(query);
    for (size_t i = 0; i < q.size();) {
        const size_t j = q.find(' ', i);
        const size_t e = j == std::string::npos ? q.size() : j;
        if (e > i) words.push_back(q.substr(i, e - i));
        i = e + 1;
    }
    std::vector<int> out;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string hay = lower(list[i].name) + " " + lower(list[i].category);
        bool all = true;
        for (const auto& w : words) all = all && hay.find(w) != std::string::npos;
        if (all) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::string sanitizePresetName(const std::string& name) {
    std::string s;
    for (unsigned char c : name) s += (c < 32 || std::string("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string::npos) ? '-' : static_cast<char>(c);
    const auto b = s.find_first_not_of(" ."), e = s.find_last_not_of(" .");
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    return s.empty() ? std::string("Preset") : s;
}

bool readFileBytes(const fs::path& p, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (error) *error = "Could not open " + utf8(p.filename()) + ".";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFileBytes(const fs::path& p, const std::vector<uint8_t>& bytes, std::string* error) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            if (error) *error = "Could not write " + utf8(p.filename()) + ".";
            return false;
        }
    }
    fs::rename(tmp, p, ec);  // replace in one step, so a failed save never leaves half a preset
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "Could not save " + utf8(p.filename()) + ".";
        return false;
    }
    return true;
}

}  // namespace amq::ui
