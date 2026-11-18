#include "amq/ui/file_dialog.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace amq::ui {

std::string pickAudioFile() {
#if defined(_WIN32)
    wchar_t buf[4096] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrTitle = L"EQ Match reference";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
#else
    return {};
#endif
}

std::string pickPresetSavePath(const std::filesystem::path& folder, const std::string& suggestedName) {
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    wchar_t buf[4096] = {};
    const int wn = MultiByteToWideChar(CP_UTF8, 0, suggestedName.c_str(), -1, buf, static_cast<int>(std::size(buf)) - 16);
    if (wn <= 0) buf[0] = 0;
    const std::wstring dir = folder.wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Am-Q preset (*.amqpreset)\0*.amqpreset\0";
    ofn.lpstrDefExt = L"amqpreset";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrTitle = L"Save Am-Q preset";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
#else
    (void)folder, (void)suggestedName;
    return {};
#endif
}

}  // namespace amq::ui
