#include "app/FileDialog.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <vector>

namespace ce {

namespace {
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}
} // namespace

std::vector<std::string> openFileDialog(const wchar_t* title, const wchar_t* filter, bool multi) {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (multi) ofn.Flags |= OFN_ALLOWMULTISELECT;

    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;

    if (multi) {
        // buffer: dir\0file1\0file2\0\0  (or single file path\0\0)
        std::wstring first = buf.data();
        const wchar_t* p = buf.data() + first.size() + 1;
        if (*p == L'\0') {
            out.push_back(toUtf8(first));
        } else {
            while (*p) {
                std::wstring f = p;
                out.push_back(toUtf8(first + L"\\" + f));
                p += f.size() + 1;
            }
        }
    } else {
        out.push_back(toUtf8(buf.data()));
    }
    return out;
}

std::string saveFileDialog(const wchar_t* title, const wchar_t* filter, const wchar_t* defExt) {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return {};
    return toUtf8(buf.data());
}

} // namespace ce

#else

namespace ce {
std::vector<std::string> openFileDialog(const wchar_t*, const wchar_t*, bool) { return {}; }
std::string saveFileDialog(const wchar_t*, const wchar_t*, const wchar_t*) { return {}; }
} // namespace ce

#endif
