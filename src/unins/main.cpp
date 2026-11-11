/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu standalone uninstaller (unins000.exe) - custom GDI UI
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <set>
#include <string>

#include "i18n.h"

static std::set<std::wstring> g_no_uninstall;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <fstream>

namespace {

std::wstring g_self_path;
std::wstring g_install_dir;
std::wstring g_log_path;

// ---- UI config from unins000.ini ----
struct UiConfig {
    std::wstring title = L"ApTgeu Uninstall";
    int width = 420;
    int height = 180;
    COLORREF background = RGB(0x1E, 0x1E, 0x1E);
    COLORREF text = RGB(0xE8, 0xE8, 0xE8);
    COLORREF button = RGB(0x2A, 0x9D, 0x8F);
    COLORREF button_text = RGB(0xFF, 0xFF, 0xFF);
    COLORREF border = RGB(0x3A, 0x3A, 0x3A);
    std::wstring confirm = L"确定要卸载此程序及其所有组件吗？";
    std::wstring yes_text = L"是(Y)";
    std::wstring no_text = L"否(N)";
};
UiConfig g_ui;

int parse_hex_color(const wchar_t* s) {
    // Accepts "0xRRGGBB" or "RRGGBB"
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    unsigned int v = 0;
    while (*s) {
        wchar_t c = *s++;
        int d;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
        else break;
        v = v * 16 + d;
    }
    int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return RGB(r, g, b);
}

std::wstring utf8_to_w(const char* s, int len) {
    if (!s || len == 0) return L"";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, len, out.data(), n);
    return out;
}

void load_ui_config() {
    std::wstring ini = g_install_dir + L"\\unins000.ini";
    HANDLE h = ::CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return; // defaults already set

    // Read entire file.
    std::string data;
    char chunk[4096];
    DWORD r = 0;
    while (::ReadFile(h, chunk, sizeof(chunk), &r, nullptr) && r > 0) data.append(chunk, r);
    ::CloseHandle(h);

    // Parse line by line.
    std::string section;
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        // Trim \r and whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim spaces around key/value.
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);

        if (section == "UninstallWindow") {
            if (key == "Title") g_ui.title = utf8_to_w(val.c_str(), (int)val.size());
            else if (key == "Width") g_ui.width = atoi(val.c_str());
            else if (key == "Height") g_ui.height = atoi(val.c_str());
        } else if (section == "UninstallColors") {
            std::wstring wv = utf8_to_w(val.c_str(), (int)val.size());
            if (key == "Background") g_ui.background = parse_hex_color(wv.c_str());
            else if (key == "Text") g_ui.text = parse_hex_color(wv.c_str());
            else if (key == "Button") g_ui.button = parse_hex_color(wv.c_str());
            else if (key == "ButtonText") g_ui.button_text = parse_hex_color(wv.c_str());
            else if (key == "Border") g_ui.border = parse_hex_color(wv.c_str());
        } else if (section == "UninstallText") {
            std::wstring wv = utf8_to_w(val.c_str(), (int)val.size());
            if (key == "ConfirmMessage") g_ui.confirm = wv;
            else if (key == "YesButton") g_ui.yes_text = wv;
            else if (key == "NoButton") g_ui.no_text = wv;
        }
    }
}

// Button rects (relative to client area)
RECT g_yes_rect, g_no_rect;
int g_user_choice = 0; // 1=yes, 2=no, 0=waiting
RECT g_check_rect;
bool g_preserve_config = false;

// v0.2.3: backup user config files to %APPDATA%\ApTgeu_Backup\{AppName}
std::wstring g_backup_dir;
std::vector<std::wstring> g_backed_up;

// Progress window state.
HWND g_progress_hwnd = nullptr;
int g_progress_done = 0;
int g_progress_total = 0;
std::wstring g_progress_label;

LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc; ::GetClientRect(hwnd, &rc);
        HBRUSH bg = ::CreateSolidBrush(g_ui.background);
        ::FillRect(hdc, &rc, bg); ::DeleteObject(bg);
        HFONT hf = ::CreateFontW(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        HFONT old = (HFONT)::SelectObject(hdc, hf);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, g_ui.text);
        RECT trc = {20, 20, rc.right-20, 60};
        ::DrawTextW(hdc, g_progress_label.c_str(), -1, &trc, DT_LEFT | DT_SINGLELINE);
        // Progress bar.
        RECT bar = {20, 70, rc.right-20, 95};
        HBRUSH border = ::CreateSolidBrush(g_ui.border);
        ::FillRect(hdc, &bar, border); ::DeleteObject(border);
        float frac = g_progress_total > 0 ? (float)g_progress_done / (float)g_progress_total : 0;
        RECT fill = {bar.left+2, bar.top+2, bar.left+2+(LONG)((bar.right-bar.left-4)*frac), bar.bottom-2};
        HBRUSH btn = ::CreateSolidBrush(g_ui.button);
        ::FillRect(hdc, &fill, btn); ::DeleteObject(btn);
        ::SelectObject(hdc, old); ::DeleteObject(hf);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

HWND show_progress(const std::wstring& label, int total) {
    g_progress_label = label;
    g_progress_total = total;
    g_progress_done = 0;
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ProgressWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ApTgeuUninsProgress";
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    ::RegisterClassW(&wc);
    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
    HWND h = ::CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, g_ui.title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sw-420)/2, (sh-140)/2, 420, 140, nullptr, nullptr, wc.hInstance, nullptr);
    if (h) { ::ShowWindow(h, SW_SHOW); ::UpdateWindow(h); }
    g_progress_hwnd = h;
    return h;
}

void update_progress(int done, const std::wstring& label) {
    g_progress_done = done;
    if (!label.empty()) g_progress_label = label;
    if (g_progress_hwnd) { ::InvalidateRect(g_progress_hwnd, nullptr, TRUE); ::UpdateWindow(g_progress_hwnd); }
    MSG msg; while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
}

void hide_progress() {
    if (g_progress_hwnd) { ::DestroyWindow(g_progress_hwnd); g_progress_hwnd = nullptr; }
}

void log_line(const wchar_t* fmt, ...) {
    // v0.20: append to the SAME %TEMP%\aptgeu_setup.log as the installer, UTF-8.
    HANDLE h = ::CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%04d-%02d-%02d %02d:%02d:%02d] [INFO] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD w = 0;
    ::WriteFile(h, hdr, (DWORD)strlen(hdr), &w, nullptr);
    wchar_t body[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf(body, 1024, fmt, ap);
    va_end(ap);
    int n = ::WideCharToMultiByte(CP_UTF8, 0, body, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string u8(n - 1, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, body, -1, u8.data(), n, nullptr, nullptr);
        ::WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    }
    ::WriteFile(h, "\r\n", 2, &w, nullptr);
    ::CloseHandle(h);
}

void log_error(const wchar_t* fmt, ...) {
    HANDLE h = ::CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%04d-%02d-%02d %02d:%02d:%02d] [ERROR] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD w = 0;
    ::WriteFile(h, hdr, (DWORD)strlen(hdr), &w, nullptr);
    wchar_t body[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf(body, 1024, fmt, ap);
    va_end(ap);
    int n = ::WideCharToMultiByte(CP_UTF8, 0, body, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string u8(n - 1, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, body, -1, u8.data(), n, nullptr, nullptr);
        ::WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    }
    ::WriteFile(h, "\r\n", 2, &w, nullptr);
    ::CloseHandle(h);
}

std::wstring get_self_path() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return buf;
}

std::wstring dir_of(const std::wstring& p) {
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::vector<std::wstring> read_dat() {
    std::wstring dat = g_install_dir + L"\\unins000.dat";
    std::vector<std::wstring> out;
    HANDLE h = ::CreateFileW(dat.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::string all;
    char chunk[4096];
    DWORD r = 0;
    while (::ReadFile(h, chunk, sizeof(chunk), &r, nullptr) && r > 0) {
        all.append(chunk, r);
    }
    ::CloseHandle(h);
    size_t start = 0;
    for (size_t i = 0; i <= all.size(); ++i) {
        if (i == all.size() || all[i] == '\n') {
            std::string line = all.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out.push_back(utf8_to_wide(line));
            start = i + 1;
        }
    }
    return out;
}

bool delete_file(const std::wstring& p) {
    ::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    return ::DeleteFileW(p.c_str()) != 0;
}

// H-01 (CWE-22): unins000.dat is a plaintext manifest in the install dir.
// A tampered line must never cause us to delete outside the install tree.
// Legitimate entries are either absolute paths under the install dir or the
// absolute .lnk shortcut paths created by the installer (Start Menu / Desktop /
// Quick Launch) - none contain a ".." segment. Any ".." segment is treated as
// a traversal attempt and refused.
bool has_dotdot_segment(const std::wstring& p) {
    for (size_t pos = 0; pos <= p.size(); ) {
        size_t slash = p.find_first_of(L"\\/", pos);
        std::wstring comp = p.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
        if (comp == L"..") return true;
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }
    return false;
}

void remove_tree(const std::wstring& dir) {
    std::wstring pat = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_tree(full);
            ::RemoveDirectoryW(full.c_str());
        } else {
            delete_file(full);
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    ::RemoveDirectoryW(dir.c_str());
}

void delete_registry_key(const std::wstring& reg_key_name) {
    std::wstring sub = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + reg_key_name;
    ::RegDeleteKeyW(HKEY_CURRENT_USER, sub.c_str());
}

void schedule_self_delete() {
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    wchar_t pidbuf[16];
    _swprintf(pidbuf, L"aptgeu_cleanup_%lu.bat", ::GetCurrentProcessId());
    std::wstring bat = std::wstring(tmp) + pidbuf;

    char buf[4096];
    int n = ::WideCharToMultiByte(CP_ACP, 0, g_self_path.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    std::string self_s(buf, n > 0 ? n - 1 : 0);
    n = ::WideCharToMultiByte(CP_ACP, 0, g_install_dir.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    std::string dir_s(buf, n > 0 ? n - 1 : 0);

    std::ofstream f(bat.c_str(), std::ios::binary | std::ios::trunc);
    f << "@echo off\r\n:loop\r\n";
    f << "del /f /q \"" << self_s << "\" 2>nul\r\n";
    f << "if exist \"" << self_s << "\" (\r\n  ping -n 2 127.0.0.1 >nul\r\n  goto loop\r\n)\r\n";
    f << "rmdir /s /q \"" << dir_s << "\"\r\n";

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    std::wstring params = L"/c \"" + bat + L"\"";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    ::ShellExecuteExW(&sei);
}

bool dir_exists(const std::wstring& p) {
    DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- v0.2.3: config-file backup helpers ----
std::wstring get_appdata_dir() {
    wchar_t buf[MAX_PATH] = {0};
    if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK)
        return L"";
    return buf;
}

std::wstring leaf_name(const std::wstring& p) {
    size_t s = p.find_last_of(L'\\');
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

bool ensure_dir_recursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (dir_exists(dir)) return true;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash >= 2) {
        if (!ensure_dir_recursive(dir.substr(0, slash))) return false;
    }
    return ::CreateDirectoryW(dir.c_str(), nullptr) != 0 ||
           ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool is_config_extension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    return ext == L".ini" || ext == L".cfg" || ext == L".xml" ||
           ext == L".conf" || ext == L".json";
}

// Build (once) %APPDATA%\ApTgeu_Backup\{AppName}\ where AppName = install dir leaf.
void init_backup_dir() {
    if (!g_backup_dir.empty()) return;
    std::wstring appdata = get_appdata_dir();
    if (appdata.empty()) { log_line(L"backup: APPDATA unavailable"); return; }
    g_backup_dir = appdata + L"\\ApTgeu_Backup\\" + leaf_name(g_install_dir);
    if (!ensure_dir_recursive(g_backup_dir)) {
        log_line(L"backup: cannot create dir %ls", g_backup_dir.c_str());
        g_backup_dir.clear();
    }
}

void write_backup_manifest() {
    if (g_backed_up.empty() || g_backup_dir.empty()) return;
    std::wstring mf = g_backup_dir + L"\\backup_manifest.txt";
    HANDLE h = ::CreateFileW(mf.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { log_line(L"manifest: open failed"); return; }
    // UTF-16LE BOM
    DWORD w = 0;
    unsigned char bom[2] = {0xFF, 0xFE};
    ::WriteFile(h, bom, 2, &w, nullptr);
    std::wstring content = L"# ApTgeu uninstall config backup\r\n";
    content += L"# source -> backup location\r\n";
    for (const auto& src : g_backed_up) {
        content += src;
        content += L" -> ";
        content += g_backup_dir;
        content += L"\\";
        content += leaf_name(src);
        content += L"\r\n";
    }
    ::WriteFile(h, content.c_str(), (DWORD)(content.size() * sizeof(wchar_t)), &w, nullptr);
    ::CloseHandle(h);
    log_line(L"manifest written: %ls (%d files)", mf.c_str(), (int)g_backed_up.size());
}

bool has_arg(int argc, wchar_t** argv, const wchar_t* needle) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], needle) == 0) return true;
    }
    return false;
}

// ---- System Restore point (srclient.dll, loaded dynamically) ----
//
// Mirrors stub/main.cpp's install-time restore point. We LoadLibraryW
// ("srclient.dll") + GetProcAddress("SRSetRestorePointW") at runtime so the
// standalone uninstaller still starts on SKUs where System Restore is
// disabled or when the process lacks admin rights. Any failure is
// non-fatal: we only append a line to aptgeu_unins.log and continue.

#ifndef BEGIN_SYSTEM_CHANGE
#define BEGIN_SYSTEM_CHANGE 100
#endif
#ifndef END_SYSTEM_CHANGE
#define END_SYSTEM_CHANGE 101
#endif
#ifndef APPLICATION_UNINSTALL
#define APPLICATION_UNINSTALL 1
#endif

typedef struct _ATU_RPINFO_W {
    DWORD dwEventType;
    DWORD dwRestorePtType;
    INT64 llSequenceNumber;
    WCHAR szDescription[256];
} ATU_RPINFO_W;

typedef struct _ATU_SRPINFO_W {
    DWORD dwRPNum;
} ATU_SRPINFO_W;

typedef BOOL (WINAPI *PFN_SRSetRestorePointW)(ATU_RPINFO_W*, ATU_SRPINFO_W*);

void create_restore_point_before_uninstall() {
    HMODULE hSr = ::LoadLibraryW(L"srclient.dll");
    if (!hSr) {
        log_line(L"restore point: LoadLibraryW(srclient.dll) failed, err=%d - skipped",
                 (int)::GetLastError());
        return;
    }
    auto pfn = (PFN_SRSetRestorePointW)::GetProcAddress(hSr, "SRSetRestorePointW");
    if (!pfn) {
        log_line(L"restore point: GetProcAddress failed, err=%d - skipped",
                 (int)::GetLastError());
        ::FreeLibrary(hSr);
        return;
    }

    ATU_RPINFO_W rp = {0};
    rp.dwEventType     = BEGIN_SYSTEM_CHANGE;
    rp.dwRestorePtType = APPLICATION_UNINSTALL;
    ::wcsncpy(rp.szDescription, L"ApTgeu: before uninstall", _countof(rp.szDescription) - 1);
    ATU_SRPINFO_W sp = {0};

    BOOL ok = pfn(&rp, &sp);
    if (!ok) {
        log_line(L"restore point: SRSetRestorePointW failed, err=%d seq=%lld - skipped",
                 (int)::GetLastError(), (long long)rp.llSequenceNumber);
    } else {
        log_line(L"restore point: SRSetRestorePointW ok, seq=%lld",
                 (long long)rp.llSequenceNumber);
    }
    ::FreeLibrary(hSr);
}

// ---- Custom confirm window ----
LRESULT CALLBACK UninstallWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Center window.
        int sw = ::GetSystemMetrics(SM_CXSCREEN);
        int sh = ::GetSystemMetrics(SM_CYSCREEN);
        int x = (sw - g_ui.width) / 2;
        int y = (sh - g_ui.height) / 2;
        ::SetWindowPos(hwnd, HWND_TOP, x, y, g_ui.width, g_ui.height, SWP_SHOWWINDOW);
        // Layout buttons.
        int bw = 90, bh = 30, margin = 20, gap = 15;
        g_yes_rect.left = g_ui.width - margin - bw * 2 - gap;
        g_yes_rect.top = g_ui.height - margin - bh;
        g_yes_rect.right = g_yes_rect.left + bw;
        g_yes_rect.bottom = g_yes_rect.top + bh;
        g_no_rect.left = g_yes_rect.right + gap;
        g_no_rect.top = g_yes_rect.top;
        g_no_rect.right = g_no_rect.left + bw;
        g_no_rect.bottom = g_no_rect.top + bh;
        // checkbox rect (above buttons)
        g_check_rect.left = 25;
        g_check_rect.top = g_ui.height - margin - bh - 30;
        g_check_rect.right = g_check_rect.left + 16;
        g_check_rect.bottom = g_check_rect.top + 16;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc; ::GetClientRect(hwnd, &rc);

        // Background.
        HBRUSH bg = ::CreateSolidBrush(g_ui.background);
        ::FillRect(hdc, &rc, bg);
        ::DeleteObject(bg);

        // Border.
        HPEN pen = ::CreatePen(PS_SOLID, 1, g_ui.border);
        HPEN oldpen = (HPEN)::SelectObject(hdc, pen);
        ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        ::SelectObject(hdc, oldpen);
        ::DeleteObject(pen);

        // Confirm text.
        HFONT hfont = ::CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldfont = (HFONT)::SelectObject(hdc, hfont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_ui.text);
        RECT text_rc = {25, 30, rc.right - 25, g_yes_rect.top - 20};
        ::DrawTextW(hdc, g_ui.confirm.c_str(), -1, &text_rc, DT_WORDBREAK | DT_CENTER | DT_VCENTER);

        // Checkbox "保留用户配置文件"
        ::Rectangle(hdc, g_check_rect.left, g_check_rect.top, g_check_rect.right, g_check_rect.bottom);
        if (g_preserve_config) {
            HPEN cpen = ::CreatePen(PS_SOLID, 2, g_ui.button);
            HPEN op = (HPEN)::SelectObject(hdc, cpen);
            ::MoveToEx(hdc, g_check_rect.left + 2, g_check_rect.top + 8, nullptr);
            ::LineTo(hdc, g_check_rect.left + 6, g_check_rect.top + 12);
            ::LineTo(hdc, g_check_rect.left + 14, g_check_rect.top + 3);
            ::SelectObject(hdc, op);
            ::DeleteObject(cpen);
        }
        RECT check_text = {g_check_rect.right + 6, g_check_rect.top - 2, rc.right - 25, g_check_rect.bottom + 2};
        ::DrawTextW(hdc, atu::i18n_w("unins_preserve_config").c_str(), -1, &check_text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Yes button.
        HBRUSH btn = ::CreateSolidBrush(g_ui.button);
        ::FillRect(hdc, &g_yes_rect, btn);
        ::DeleteObject(btn);
        SetTextColor(hdc, g_ui.button_text);
        ::DrawTextW(hdc, g_ui.yes_text.c_str(), -1, &g_yes_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // No button (outlined).
        HPEN npen = ::CreatePen(PS_SOLID, 1, g_ui.border);
        HPEN op = (HPEN)::SelectObject(hdc, npen);
        HBRUSH ob = (HBRUSH)::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(hdc, g_no_rect.left, g_no_rect.top, g_no_rect.right, g_no_rect.bottom);
        ::SelectObject(hdc, op);
        ::SelectObject(hdc, ob);
        ::DeleteObject(npen);
        SetTextColor(hdc, g_ui.text);
        ::DrawTextW(hdc, g_ui.no_text.c_str(), -1, &g_no_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        ::SelectObject(hdc, oldfont);
        ::DeleteObject(hfont);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        POINT pt = {x, y};
        if (::PtInRect(&g_yes_rect, pt)) { g_user_choice = 1; ::DestroyWindow(hwnd); }
        else if (::PtInRect(&g_no_rect, pt)) { g_user_choice = 2; ::DestroyWindow(hwnd); }
        else if (::PtInRect(&g_check_rect, pt)) { g_preserve_config = !g_preserve_config; ::InvalidateRect(hwnd, nullptr, TRUE); }
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool show_confirm_dialog() {
    // v0.2.5: default UI strings come from the i18n table (selected by /LANG=
    // or the language the installer was invoked with). unins000.ini, if
    // present, overrides these per-key below.
    g_ui.title       = atu::i18n_w("unins_title");
    g_ui.confirm     = atu::i18n_w("unins_confirm_message");
    g_ui.yes_text    = atu::i18n_w("unins_yes");
    g_ui.no_text     = atu::i18n_w("unins_no");
    load_ui_config();

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = UninstallWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ApTgeuUninstallDlg";
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    ::RegisterClassW(&wc);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST, wc.lpszClassName, g_ui.title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, g_ui.width, g_ui.height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) return true; // default to yes on failure

    ::ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return g_user_choice == 1;
}

// ---- v0.24: [UninstallRun] real execution ----
//
// Inno order: after confirm, BEFORE deleting any files, run each [UninstallRun]
// entry. Then delete files, then process [UninstallDelete] / [Services] /
// [Registry] cleanup. This matches Inno Setup's standard uninstall sequence.
struct UninsRunEntry {
    std::wstring filename;    // raw (may contain {app})
    std::wstring parameters;  // raw (may contain {app})
    std::wstring workingdir;  // raw (may contain {app}); empty = install dir
    std::wstring flags;       // e.g. "runhidden skipifsourcedoesntexist"
};
static std::vector<UninsRunEntry> g_unins_run;
static std::vector<std::wstring>  g_unins_delete;   // resolved paths
static std::vector<std::wstring>  g_unins_services; // service key names
struct UninsRegLine {
    std::wstring root;     // hkcu/hklm/hkcr (lowercased at use)
    std::wstring subkey;
    std::wstring vname;
    bool delkey = false;
};
static std::vector<UninsRegLine> g_unins_reg;

// v0.24: [INI] entry to delete on uninstall.
struct UninsIniLine {
    std::wstring filename;  // absolute path (already {app}-substituted by stub)
    std::wstring section;
    std::wstring key;
};
static std::vector<UninsIniLine> g_unins_ini;

// Replace every "{app}" occurrence in `s` with the real install dir (wide).
static std::wstring substitute_app_w(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t pos = 0;
    for (;;) {
        size_t p = s.find(L"{app}", pos);
        if (p == std::wstring::npos) { out.append(s, pos, std::wstring::npos); break; }
        out.append(s, pos, p - pos);
        out.append(g_install_dir);
        pos = p + 5;
    }
    return out;
}

// Split a wire line on '|' into UTF-8 fields (trailing empty fields preserved).
static std::vector<std::string> split_pipe(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '|') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Read unins000_extra.ini once, populate g_unins_run / g_no_uninstall /
// g_unins_delete / g_unins_services / g_unins_reg. Safe to call when the file
// is missing (all vectors stay empty).
static void parse_extra_ini() {
    g_unins_run.clear();
    g_unins_delete.clear();
    g_unins_services.clear();
    g_unins_reg.clear();
    g_unins_ini.clear();

    std::wstring extra = g_install_dir + L"\\unins000_extra.ini";
    if (GetFileAttributesW(extra.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    HANDLE h = ::CreateFileW(extra.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text;
    char chunk[4096];
    DWORD r = 0;
    while (::ReadFile(h, chunk, sizeof(chunk), &r, nullptr) && r > 0) text.append(chunk, r);
    ::CloseHandle(h);

    std::string section;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '\n') continue;
        std::string line = text.substr(start, i - start);
        start = i + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // trim
        size_t lb = line.find_first_not_of(" \t");
        if (lb == std::string::npos) continue;
        line = line.substr(lb);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        if (section == "UninstallRun") {
            // wire: filename|parameters|workingdir|flags  (all UTF-8)
            auto parts = split_pipe(line);
            while (parts.size() < 4) parts.push_back("");
            UninsRunEntry e;
            e.filename   = utf8_to_wide(parts[0]);
            e.parameters = utf8_to_wide(parts[1]);
            e.workingdir = utf8_to_wide(parts[2]);
            e.flags      = utf8_to_wide(parts[3]);
            g_unins_run.push_back(std::move(e));
        } else if (section == "NoUninstall") {
            // one UTF-8 path per line; may contain {app}.
            std::wstring w = substitute_app_w(utf8_to_wide(line));
            g_no_uninstall.insert(w);
        } else if (section == "UninstallDelete") {
            // type|name
            auto parts = split_pipe(line);
            if (parts.size() < 2) continue;
            std::wstring name = substitute_app_w(utf8_to_wide(parts[1]));
            g_unins_delete.push_back(name);
        } else if (section == "Services") {
            std::string svc = line;
            while (!svc.empty() && (svc.back()==' ' || svc.back()=='\t')) svc.pop_back();
            while (!svc.empty() && (svc.front()==' ' || svc.front()=='\t')) svc.erase(0,1);
            if (!svc.empty()) g_unins_services.push_back(utf8_to_wide(svc));
        } else if (section == "Registry") {
            // root|subkey|valuetype|valuename|valuedata|uninsdeletekey
            auto parts = split_pipe(line);
            if (parts.size() < 5) continue;
            UninsRegLine r;
            r.root    = utf8_to_wide(parts[0]);
            r.subkey  = utf8_to_wide(parts[1]);
            r.vname   = utf8_to_wide(parts[3]);
            r.delkey  = (parts.size() >= 6 && parts[5] == "1");
            g_unins_reg.push_back(std::move(r));
        } else if (section == "INI") {
            // v0.24: filename|section|key|value|uninsdeleteentry(1/0)
            // The stub already substituted {app}; only keep entries flagged
            // uninsdeleteentry=1.
            auto parts = split_pipe(line);
            if (parts.size() < 5) continue;
            if (parts[4] != "1") continue;
            UninsIniLine r;
            r.filename = utf8_to_wide(parts[0]);
            r.section  = utf8_to_wide(parts[1]);
            r.key      = utf8_to_wide(parts[2]);
            g_unins_ini.push_back(std::move(r));
        }
    }
    log_line(L"extra.ini parsed: run=%d delfiles=%d svc=%d reg=%d ini=%d nouninst=%d",
             (int)g_unins_run.size(), (int)g_unins_delete.size(),
             (int)g_unins_services.size(), (int)g_unins_reg.size(),
             (int)g_unins_ini.size(),
             (int)g_no_uninstall.size());
}

// Run one resolved [UninstallRun] entry via CreateProcessW, wait for it.
static void launch_unins_run_entry(const UninsRunEntry& raw) {
    std::wstring filename   = substitute_app_w(raw.filename);
    std::wstring parameters = substitute_app_w(raw.parameters);
    std::wstring workingdir = substitute_app_w(raw.workingdir);

    bool hidden = raw.flags.find(L"runhidden") != std::wstring::npos;
    bool skip_if_missing = raw.flags.find(L"skipifsourcedoesntexist") != std::wstring::npos;

    if (workingdir.empty()) workingdir = g_install_dir;

    // skipifsourcedoesntexist: if the target file is gone, silently skip.
    // (Resolve to a path that exists on disk; if Filename is a full path that
    // exists, fine. If it's relative, resolve against WorkingDir first.)
    std::wstring target = filename;
    if (target.find(L'\\') == std::wstring::npos && target.find(L'/') == std::wstring::npos) {
        target = workingdir + L"\\" + target;
    }
    DWORD attr = ::GetFileAttributesW(target.c_str());
    bool exists = (attr != INVALID_FILE_ATTRIBUTES);
    if (!exists) {
        // Maybe Filename is a PATH-resolvable command (e.g. "cmd.exe"). Try
        // CreateProcess below; skipifsourcedoesntexist only suppresses the
        // error log if we cannot even stat the target.
        if (skip_if_missing) {
            log_line(L"UninstallRun: skip (source missing): %ls", target.c_str());
            return;
        }
        log_error(L"UninstallRun: source not found: %ls", target.c_str());
        // fall through: let CreateProcess attempt and log the error
    }

    // Build command line: "\"filename\" parameters". CreateProcessW mutates
    // the buffer, so we need a mutable copy.
    std::wstring cmd = L"\"" + filename + L"\"";
    if (!parameters.empty()) {
        cmd += L" ";
        cmd += parameters;
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    log_line(L"UninstallRun: launching %ls %ls (wd=%ls, hidden=%d)",
             filename.c_str(), parameters.c_str(), workingdir.c_str(), (int)hidden);

    BOOL ok = ::CreateProcessW(
        nullptr,            // lpApplicationName (use command line)
        buf.data(),         // lpCommandLine (mutable)
        nullptr, nullptr,   // security attrs
        FALSE,              // inherit handles
        hidden ? CREATE_NO_WINDOW : 0,
        nullptr,            // env
        workingdir.c_str(),
        &si, &pi);

    if (!ok) {
        DWORD e = ::GetLastError();
        log_error(L"UninstallRun: CreateProcessW failed err=%d: %ls", (int)e, filename.c_str());
        return;
    }

    // Wait for completion (Inno default is to wait; no nowait flag requested).
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    log_line(L"UninstallRun: exit=%u: %ls", (unsigned)exit_code, filename.c_str());

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
}

static void run_unins_run_entries() {
    for (const auto& e : g_unins_run) launch_unins_run_entry(e);
}

// Post-file-deletion cleanup: [UninstallDelete], [Services], [Registry].
static void run_unins_cleanup() {
    // [UninstallDelete] - best effort delete files / empty dirs.
    for (const auto& path : g_unins_delete) {
        if (::DeleteFileW(path.c_str())) {
            log_line(L"UninstallDelete: removed file %ls", path.c_str());
        } else {
            // Try as directory.
            if (::RemoveDirectoryW(path.c_str())) {
                log_line(L"UninstallDelete: removed dir %ls", path.c_str());
            } else {
                log_line(L"UninstallDelete: gone (err=%d): %ls",
                         (int)::GetLastError(), path.c_str());
            }
        }
    }

    // [Services] - stop + delete.
    for (const auto& wsvc : g_unins_services) {
        SC_HANDLE scm = ::OpenSCManagerW(nullptr, L"ServicesActive", SC_MANAGER_CONNECT);
        if (!scm) {
            log_error(L"unins: OpenSCManagerW failed err=%d", (int)::GetLastError());
            continue;
        }
        SC_HANDLE svc = ::OpenServiceW(scm, wsvc.c_str(), DELETE);
        if (!svc) {
            log_line(L"unins: OpenServiceW(%ls) err=%d (already gone?)",
                     wsvc.c_str(), (int)::GetLastError());
        } else {
            SERVICE_STATUS ss;
            SC_HANDLE h = ::OpenServiceW(scm, wsvc.c_str(), SERVICE_STOP);
            if (h) { ::ControlService(h, SERVICE_CONTROL_STOP, &ss); ::CloseServiceHandle(h); }
            if (::DeleteService(svc)) {
                log_line(L"unins: DeleteService(%ls) ok", wsvc.c_str());
            } else {
                log_error(L"unins: DeleteService(%ls) err=%d", wsvc.c_str(), (int)::GetLastError());
            }
            ::CloseServiceHandle(svc);
        }
        ::CloseServiceHandle(scm);
    }

    // [Registry] - delete value or recursive key.
    for (const auto& r : g_unins_reg) {
        HKEY hRoot = nullptr;
        std::wstring root = r.root;
        for (auto& c : root) c = (wchar_t)towlower(c);
        if (root == L"hkcu") hRoot = HKEY_CURRENT_USER;
        else if (root == L"hklm") hRoot = HKEY_LOCAL_MACHINE;
        else if (root == L"hkcr") hRoot = HKEY_CLASSES_ROOT;
        if (!hRoot) {
            log_error(L"unins: unknown registry root %ls", r.root.c_str());
            continue;
        }
        if (r.delkey) {
            std::function<bool(HKEY, const std::wstring)> del_tree;
            del_tree = [&](HKEY h, const std::wstring& path) -> bool {
                HKEY hh;
                if (::RegOpenKeyExW(h, path.c_str(), 0,
                                    KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &hh) == ERROR_SUCCESS) {
                    wchar_t subname[256];
                    DWORD idx = 0;
                    DWORD cchName = _countof(subname);
                    while (::RegEnumKeyExW(hh, idx, subname, &cchName,
                                           nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                        del_tree(h, path + L"\\" + subname);
                        cchName = _countof(subname);
                    }
                    ::RegCloseKey(hh);
                }
                LSTATUS st = ::RegDeleteKeyExW(h, path.c_str(), KEY_WOW64_64KEY, 0);
                if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
                    log_error(L"unins: RegDeleteKeyExW(%ls) rc=%d", path.c_str(), (int)st);
                    return false;
                }
                return true;
            };
            del_tree(hRoot, r.subkey);
            log_line(L"unins: deleted registry key %ls\\%ls", r.root.c_str(), r.subkey.c_str());
        } else {
            HKEY h;
            LSTATUS st = ::RegOpenKeyExW(hRoot, r.subkey.c_str(), 0,
                                         KEY_SET_VALUE | KEY_WOW64_64KEY, &h);
            if (st == ERROR_SUCCESS) {
                st = ::RegDeleteValueW(h, r.vname.empty() ? nullptr : r.vname.c_str());
                ::RegCloseKey(h);
            }
            if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND || st == ERROR_PATH_NOT_FOUND) {
                log_line(L"unins: deleted registry value %ls\\%ls\\%ls",
                         r.root.c_str(), r.subkey.c_str(), r.vname.c_str());
            } else {
                log_error(L"unins: RegDeleteValueW rc=%d", (int)st);
            }
        }
    }

    // v0.24: [INI] - delete each key via WritePrivateProfileStringW(NULL).
    // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND are tolerated (the INI file
    // may already have been removed by the tree deletion).
    for (const auto& r : g_unins_ini) {
        BOOL ok = ::WritePrivateProfileStringW(
            r.section.c_str(), r.key.c_str(), nullptr, r.filename.c_str());
        DWORD e = ::GetLastError();
        if (ok || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            log_line(L"unins: deleted INI entry [%ls]%ls from %ls",
                     r.section.c_str(), r.key.c_str(), r.filename.c_str());
        } else {
            log_error(L"unins: WritePrivateProfileStringW(delete) rc=%d", (int)e);
        }
    }
}

} // namespace

int main() {
    g_self_path = get_self_path();
    if (g_self_path.empty()) return 1;
    g_install_dir = dir_of(g_self_path);

    // B6: single-instance mutex.
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"ApTgeu_Uninstall_ApTgeu_FileTgie");
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // NOTE: i18n not set up yet here; fall back to a neutral English string.
        ::MessageBoxW(nullptr, L"Uninstaller is already running", L"ApTgeu Uninstall", MB_ICONINFORMATION);
        return 0;
    }

    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    g_log_path = std::wstring(tmp) + L"aptgeu_setup.log";
    log_line(L"=== unins000 start ===");

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    bool silent = has_arg(argc, argv, L"/SILENT") || has_arg(argc, argv, L"/VERYSILENT");
    // Hidden self-test hook: force "preserve config" ON and skip the interactive
    // confirm dialog, so the backup code path can be exercised end-to-end.
    bool self_test_backup = has_arg(argc, argv, L"/SELFTESTBACKUP");
    // v0.2.5: /LANG=xx selects the UI language (zh/en/ja/ko/ru/de/fr/es/it/pt-BR).
    // The installer writes this into the UninstallString registry value so the
    // standalone uninstaller matches the language the user picked at install time.
    std::string lang_override;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > 6 && _wcsnicmp(a.c_str(), L"/LANG=", 6) == 0) {
            std::wstring wl = a.substr(6);
            // strip quotes
            if (!wl.empty() && wl.front() == L'"') wl.erase(0, 1);
            if (!wl.empty() && wl.back()  == L'"') wl.pop_back();
            lang_override.assign(wl.begin(), wl.end());
            break;
        }
    }
    ::LocalFree(argv);

    atu::i18n_set_language(lang_override);
    log_line(L"unins language: %hs", lang_override.c_str());

    if (self_test_backup) {
        g_preserve_config = true;
        log_line(L"self-test backup mode: preserve=on");
    } else if (!silent) {
        bool yes = show_confirm_dialog();
        if (!yes) {
            log_line(L"user cancelled");
            return 0;
        }
    }

    // v0.2.4: create a system restore point before deleting anything
    // (best-effort, non-fatal; mirrors install-side restore point).
    create_restore_point_before_uninstall();

    auto files = read_dat();
    log_line(L"dat entries: %d", (int)files.size());

    // v0.24: parse unins000_extra.ini ONCE, up front. This populates
    // g_no_uninstall (consulted by the file-deletion loop below),
    // g_unins_run (executed BEFORE deleting files - Inno order), and
    // g_unins_delete / g_unins_services / g_unins_reg (executed AFTER).
    parse_extra_ini();
    run_unins_run_entries();

    const wchar_t* kRegKey = L"ApTgeu_FileTgie";
    int deleted = 0, failed = 0;
    std::wstring deleting_label = atu::i18n_w("unins_deleting_files");
    if (!silent && !files.empty()) show_progress(deleting_label.c_str(), (int)files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        // H-01: refuse to act on a traversal path recorded in unins000.dat.
        if (has_dotdot_segment(files[i])) {
            log_error(L"refusing to delete traversal path in unins000.dat: %ls", files[i].c_str());
            continue;
        }
        // v0.19: skip files marked uninsneveruninstall
        if (g_no_uninstall.count(files[i])) {
            log_line(L"kept (uninsneveruninstall): %ls", files[i].c_str());
            continue;
        }
        // v0.16: skip config files if "preserve" checkbox checked
        // v0.2.3: instead of merely skipping, CopyFileW them into
        //         %APPDATA%\ApTgeu_Backup\{AppName}\ and record the manifest.
        if (g_preserve_config && is_config_extension(files[i])) {
            init_backup_dir();
            if (!g_backup_dir.empty()) {
                std::wstring dst = g_backup_dir + L"\\" + leaf_name(files[i]);
                if (::CopyFileW(files[i].c_str(), dst.c_str(), FALSE)) {
                    g_backed_up.push_back(files[i]);
                    log_line(L"backed up: %ls -> %ls", files[i].c_str(), dst.c_str());
                } else {
                    log_error(L"backup failed (%d): %ls", (int)::GetLastError(), files[i].c_str());
                }
            }
            log_line(L"preserved: %ls", files[i].c_str());
            continue;
        }
        if (delete_file(files[i])) deleted++;
        else { failed++; log_error(L"delete failed: %ls", files[i].c_str()); }
        if (!silent) update_progress((int)(i+1), deleting_label);
    }
    if (!silent) hide_progress();
    log_line(L"files deleted: %d, failed: %d", deleted, failed);

    // v0.2.3: write backup_manifest.txt listing every config file copied out.
    if (g_preserve_config && !g_backed_up.empty()) write_backup_manifest();

    // v0.24: post-file-deletion cleanup ([UninstallDelete], [Services], [Registry]).
    // [UninstallRun] already ran BEFORE the deletion loop above (Inno order).
    run_unins_cleanup();


    delete_registry_key(kRegKey);
    remove_tree(g_install_dir);

    // v0.2.3: tell the user where the preserved config files went (GUI mode only;
    // silent installs must never pop a modal box that would hang automation).
    if (g_preserve_config && !g_backed_up.empty() && !silent && !self_test_backup) {
        std::wstring msg = atu::i18n_w("unins_backup_done") + L"\n" + g_backup_dir;
        ::MessageBoxW(nullptr, msg.c_str(), g_ui.title.c_str(), MB_OK | MB_ICONINFORMATION);
    }

    ::MoveFileExW(g_install_dir.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    ::MoveFileExW(g_self_path.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    schedule_self_delete();

    int waited_ms = 0;
    while (waited_ms < 5000 && dir_exists(g_install_dir)) {
        ::Sleep(200);
        waited_ms += 200;
    }
    log_line(L"install dir %s after %d ms",
             dir_exists(g_install_dir) ? "STILL EXISTS" : "removed", waited_ms);
    log_line(L"=== unins000 done ===");
    return 0;
}
