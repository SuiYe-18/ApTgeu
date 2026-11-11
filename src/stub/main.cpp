/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu stub - WinMain entry.
//
// Responsibilities:
//   * Parse command line (/SILENT, /VERYSILENT, /DIR=..., /UNINSTALL)
//   * Read payload from own exe footer
//   * Dispatch to: silent install, uninstall mode, or GUI wizard
//   * GUI: create Win32 window + OpenGL3 context, run ImGui frame loop

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <GL/gl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#include "unpack.h"
#include "registry.h"
#include "ini.h"
#include "gui_pages.h"
#include "i18n.h"
#include "log.h"

// Forward declaration from imgui_impl_win32.cpp (must be copied per its header).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// WGL boilerplate (mirrors imgui example_win32_opengl3)
// ---------------------------------------------------------------------------

struct WGL_WindowData { HDC hDC; };
static HGLRC          g_hRC;
static WGL_WindowData g_MainWindow;
static int            g_Width  = 800;
static int            g_Height = 500;

static bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data);
static void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_Width  = LOWORD(lParam);
            g_Height = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // disable ALT menu
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// System Restore point (srclient.dll, loaded dynamically)
// ---------------------------------------------------------------------------
//
// We LoadLibraryW("srclient.dll") + GetProcAddress("SRSetRestorePointW")
// at runtime instead of linking srclient.lib, so the stub still starts on
// SKUs where System Restore is disabled (e.g. some Server editions) or when
// the process lacks admin rights. Any failure here is non-fatal: we only
// append a [WARN] line to %TEMP%\aptgeu_setup_log.txt and continue.

#ifndef BEGIN_SYSTEM_CHANGE
#define BEGIN_SYSTEM_CHANGE 100
#endif
#ifndef END_SYSTEM_CHANGE
#define END_SYSTEM_CHANGE 101
#endif
#ifndef APPLICATION_INSTALL
#define APPLICATION_INSTALL 0
#endif
#ifndef APPLICATION_UNINSTALL
#define APPLICATION_UNINSTALL 1
#endif

typedef struct _ATU_RESTOREPOINTINFOW {
    DWORD dwEventType;
    DWORD dwRestorePtType;
    INT64 llSequenceNumber;
    WCHAR szDescription[256];
} ATU_RESTOREPOINTINFOW;

typedef struct _ATU_SETRESTOREPOINTINFOW {
    DWORD dwRPNum;
} ATU_SETRESTOREPOINTINFOW;

typedef BOOL (WINAPI *PFN_SRSetRestorePointW)(
    ATU_RESTOREPOINTINFOW*, ATU_SETRESTOREPOINTINFOW*);

static void atu_log_warning(const std::wstring& msg) {
    // v0.20: route through atu::log (UTF-8, timestamped, %TEMP%\aptgeu_setup.log).
    std::string u8(msg.begin(), msg.end());  // narrow fallback (ASCII/Latin-1)
    atu::log_write(atu::LOG_WARN, __FILE__, __LINE__, u8);
    // Keep the legacy UTF-16 append for back-compat.
    wchar_t tmp[MAX_PATH] = {0};
    if (!::GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring path = std::wstring(tmp) + L"aptgeu_setup_log.txt";
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    wchar_t hdr[80];
    wsprintfW(hdr, L"[%04d-%02d-%02d %02d:%02d:%02d] [WARN] ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(hdr) + msg + L"\r\n";
    DWORD w = 0;
    ::WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &w, nullptr);
    ::CloseHandle(h);
}

static void atu_create_restore_point(const wchar_t* description, bool is_uninstall) {
    HMODULE hSr = ::LoadLibraryW(L"srclient.dll");
    if (!hSr) {
        atu_log_warning(std::wstring(L"LoadLibraryW(srclient.dll) failed, err=")
                        + std::to_wstring(::GetLastError())
                        + L" - restore point not created: " + description);
        return;
    }
    auto pfn = (PFN_SRSetRestorePointW)::GetProcAddress(hSr, "SRSetRestorePointW");
    if (!pfn) {
        atu_log_warning(std::wstring(L"GetProcAddress(SRSetRestorePointW) failed, err=")
                        + std::to_wstring(::GetLastError())
                        + L" - restore point not created: " + description);
        ::FreeLibrary(hSr);
        return;
    }

    ATU_RESTOREPOINTINFOW rp = {0};
    rp.dwEventType    = BEGIN_SYSTEM_CHANGE;
    rp.dwRestorePtType = is_uninstall ? APPLICATION_UNINSTALL : APPLICATION_INSTALL;
    wcsncpy_s(rp.szDescription, description, _countof(rp.szDescription) - 1);
    ATU_SETRESTOREPOINTINFOW sp = {0};

    BOOL ok = pfn(&rp, &sp);
    if (!ok) {
        atu_log_warning(std::wstring(L"SRSetRestorePointW failed, err=")
                        + std::to_wstring(::GetLastError())
                        + L" seq=" + std::to_wstring((long long)rp.llSequenceNumber)
                        + L" - restore point not created: " + description);
    } else {
        atu_log_warning(std::wstring(L"SRSetRestorePointW ok, seq=")
                        + std::to_wstring((long long)rp.llSequenceNumber)
                        + L" desc: " + description);
    }
    ::FreeLibrary(hSr);
}

// ---------------------------------------------------------------------------
// Auto-update check (wininet.dll, loaded dynamically)
// ---------------------------------------------------------------------------
//
// After a GUI install finishes, if [Setup] UpdateURL=... is set we fetch the
// plain-text version string at that URL and compare it with AppVersion.
// wininet.dll is LoadLibraryW'd at runtime (same pattern as srclient.dll) so
// we do not need to add -lwininet to build.bat. Any failure (no network,
// missing DLL, non-HTTP URL, unparseable version) is non-fatal: we just log
// a [WARN] and skip. Silent installs never call this.

#include <wininet.h>

typedef HINTERNET (WINAPI *PFN_InternetOpenW)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *PFN_InternetOpenUrlW)(HINTERNET, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD);
typedef BOOL      (WINAPI *PFN_InternetReadFile)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL      (WINAPI *PFN_InternetCloseHandle)(HINTERNET);

#ifndef INTERNET_OPEN_TYPE_PRECONFIG
#define INTERNET_OPEN_TYPE_PRECONFIG 0
#endif
#ifndef INTERNET_FLAG_RELOAD
#define INTERNET_FLAG_RELOAD 0x80000000
#endif
#ifndef INTERNET_FLAG_NO_CACHE_WRITE
#define INTERNET_FLAG_NO_CACHE_WRITE 0x04000000
#endif

// Compare dotted version strings "a.b.c" vs "x.y.z". Returns >0 if a>b,
// <0 if a<b, 0 if equal. Non-numeric segments are treated as 0.
static int atu_compare_version(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    for (;;) {
        size_t da = a.find('.', ia);
        size_t db = b.find('.', ib);
        std::string sa = (da == std::string::npos) ? a.substr(ia) : a.substr(ia, da - ia);
        std::string sb = (db == std::string::npos) ? b.substr(ib) : b.substr(ib, db - ib);
        // Trim whitespace / CR / LF.
        while (!sa.empty() && (sa.back() == '\r' || sa.back() == '\n' || sa.back() == ' ')) sa.pop_back();
        while (!sb.empty() && (sb.back() == '\r' || sb.back() == '\n' || sb.back() == ' ')) sb.pop_back();
        while (!sa.empty() && (sa.front() == ' ')) sa.erase(0, 1);
        while (!sb.empty() && (sb.front() == ' ')) sb.erase(0, 1);
        long va = sa.empty() ? 0 : ::strtol(sa.c_str(), nullptr, 10);
        long vb = sb.empty() ? 0 : ::strtol(sb.c_str(), nullptr, 10);
        if (va != vb) return va > vb ? 1 : -1;
        if (da == std::string::npos && db == std::string::npos) return 0;
        ia = (da == std::string::npos) ? a.size() : da + 1;
        ib = (db == std::string::npos) ? b.size() : db + 1;
    }
}

static void atu_check_for_updates(const std::string& update_url_utf8,
                                  const std::string& current_version) {
    if (update_url_utf8.empty()) return;
    std::wstring url = stub::utf8_to_wide(update_url_utf8);

    HMODULE hWin = ::LoadLibraryW(L"wininet.dll");
    if (!hWin) {
        atu_log_warning(std::wstring(L"LoadLibraryW(wininet.dll) failed, err=")
                        + std::to_wstring(::GetLastError()) + L" - update check skipped");
        return;
    }
    auto pOpen  = (PFN_InternetOpenW)::GetProcAddress(hWin, "InternetOpenW");
    auto pOpenUrl = (PFN_InternetOpenUrlW)::GetProcAddress(hWin, "InternetOpenUrlW");
    auto pRead  = (PFN_InternetReadFile)::GetProcAddress(hWin, "InternetReadFile");
    auto pClose = (PFN_InternetCloseHandle)::GetProcAddress(hWin, "InternetCloseHandle");
    if (!pOpen || !pOpenUrl || !pRead || !pClose) {
        atu_log_warning(L"GetProcAddress(wininet) failed - update check skipped");
        ::FreeLibrary(hWin);
        return;
    }

    HINTERNET hSession = pOpen(L"ApTgeuSetup",
                               INTERNET_OPEN_TYPE_PRECONFIG,
                               nullptr, nullptr, 0);
    if (!hSession) {
        atu_log_warning(std::wstring(L"InternetOpenW failed, err=")
                        + std::to_wstring(::GetLastError()) + L" - update check skipped");
        ::FreeLibrary(hWin);
        return;
    }
    HINTERNET hUrl = pOpenUrl(hSession, url.c_str(), nullptr, 0,
                              INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        atu_log_warning(std::wstring(L"InternetOpenUrlW failed, err=")
                        + std::to_wstring(::GetLastError()) + L" - update check skipped: " + url);
        pClose(hSession);
        ::FreeLibrary(hWin);
        return;
    }

    // Read body into a std::string.
    std::string body;
    char chunk[2048];
    DWORD got = 0;
    while (pRead(hUrl, chunk, sizeof(chunk), &got) && got > 0) {
        body.append(chunk, got);
        if (body.size() > 64 * 1024) break;  // version file should be tiny
    }
    pClose(hUrl);
    pClose(hSession);
    ::FreeLibrary(hWin);

    // Trim whitespace / BOM / CR / LF to get the version token.
    size_t s = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (s == std::string::npos) {
        atu_log_warning(L"update check: empty version response - skipped");
        return;
    }
    size_t e = body.find_first_of(" \t\r\n", s);
    std::string remote = (e == std::string::npos) ? body.substr(s) : body.substr(s, e - s);

    atu_log_warning(std::wstring(L"update check: local=")
                    + stub::utf8_to_wide(current_version)
                    + L" remote=" + stub::utf8_to_wide(remote));

    if (atu_compare_version(remote, current_version) <= 0) {
        return;  // up-to-date or unparseable: silently skip
    }

    std::wstring msg = L"发现新版本: ";
    msg += stub::utf8_to_wide(remote);
    msg += L" (当前 ";
    msg += stub::utf8_to_wide(current_version);
    msg += L")\n是否前往下载？";
    int r = ::MessageBoxW(nullptr, msg.c_str(), L"ApTgeu Setup",
                          MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES) {
        ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---------------------------------------------------------------------------
// v0.22: VC++ 2015-2022 x64 Redistributable auto-detect / silent install
// ---------------------------------------------------------------------------
//
// Detection key (documented by Microsoft):
//   HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64
//       Installed (DWORD) = 1
//       Version   (REG_SZ) = e.g. "14.38.33130.0"
// On 64-bit Windows the x64 runtime lives in the 64-bit view; the Wow6432Node
// view is also probed to stay robust against redirected registrations.
//
// If the runtime is missing and the payload carries
// "APTGEU::vcredist_x64.exe", we extract it to %TEMP% and run:
//   vcredist_x64.exe /install /quiet /norestart
// Wait up to 10 minutes. Failure is logged but never aborts the installer.

static bool detect_vcredist_x64(std::string& version_out) {
    const wchar_t* subkey = L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64";
    DWORD views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (DWORD view : views) {
        HKEY h = nullptr;
        LONG rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                                  KEY_READ | view, &h);
        if (rc != ERROR_SUCCESS) continue;
        DWORD installed = 0;
        DWORD sz = sizeof(installed);
        DWORD type = 0;
        ::RegQueryValueExW(h, L"Installed", nullptr, &type,
                           reinterpret_cast<LPBYTE>(&installed), &sz);
        wchar_t ver[256] = {0};
        DWORD ver_sz = sizeof(ver);
        ::RegQueryValueExW(h, L"Version", nullptr, nullptr,
                           reinterpret_cast<LPBYTE>(ver), &ver_sz);
        ::RegCloseKey(h);
        if (installed == 1) {
            size_t n = ::wcsnlen(ver, _countof(ver));
            version_out.assign(ver, ver + n);
            return true;
        }
    }
    return false;
}

// Run a vcredist bootstrapper at exe_path with /install /quiet /norestart,
// wait up to 10 minutes, then delete the file. Never throws / never aborts
// the outer install: every failure is logged as [WARN] and we return.
static void atu_run_vcredist_bootstrapper(const std::wstring& exe_path) {
    std::wstring cmd = L"\"" + exe_path + L"\" /install /quiet /norestart";
    STARTUPINFOW si; ::ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ::ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(L'\0');
    BOOL ok = ::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                               0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        atu::log_warn(std::string("vcredist: CreateProcessW failed, err=") +
                      std::to_string(::GetLastError()) +
                      " - continuing without vcredist");
        ::DeleteFileW(exe_path.c_str());
        return;
    }
    atu::log_info("vcredist: waiting for bootstrapper (up to 10 minutes)...");
    DWORD wait = ::WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (wait == WAIT_TIMEOUT) {
        atu::log_warn("vcredist: wait timed out after 10 minutes");
    } else {
        atu::log_info("vcredist: bootstrapper exited, code=" +
                      std::to_string(exit_code));
    }
    ::DeleteFileW(exe_path.c_str());
}

// v0.23: download a URL to a local file using wininet.dll (loaded dynamically,
// same pattern as the update check). Returns true on success. On failure
// fills err (UTF-8) and returns false. Any network / missing-DLL failure is
// non-fatal for the outer installer.
static bool atu_download_file(const std::wstring& url,
                              const std::wstring& dst_path,
                              std::string& err) {
    HMODULE hWin = ::LoadLibraryW(L"wininet.dll");
    if (!hWin) {
        err = "LoadLibraryW(wininet.dll) failed, err=" +
              std::to_string(::GetLastError());
        return false;
    }
    auto pOpen    = (PFN_InternetOpenW)::GetProcAddress(hWin, "InternetOpenW");
    auto pOpenUrl = (PFN_InternetOpenUrlW)::GetProcAddress(hWin, "InternetOpenUrlW");
    auto pRead    = (PFN_InternetReadFile)::GetProcAddress(hWin, "InternetReadFile");
    auto pClose   = (PFN_InternetCloseHandle)::GetProcAddress(hWin, "InternetCloseHandle");
    if (!pOpen || !pOpenUrl || !pRead || !pClose) {
        err = "GetProcAddress(wininet) failed";
        ::FreeLibrary(hWin);
        return false;
    }

    HINTERNET hSession = pOpen(L"ApTgeuSetup",
                               INTERNET_OPEN_TYPE_PRECONFIG,
                               nullptr, nullptr, 0);
    if (!hSession) {
        err = "InternetOpenW failed, err=" + std::to_string(::GetLastError());
        ::FreeLibrary(hWin);
        return false;
    }
    // InternetOpenUrlW follows HTTP redirects (e.g. aka.ms -> download link)
    // by default; NO_CACHE_WRITE avoids polluting the shared WinINet cache.
    HINTERNET hUrl = pOpenUrl(hSession, url.c_str(), nullptr, 0,
                              INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        err = "InternetOpenUrlW failed, err=" + std::to_string(::GetLastError());
        pClose(hSession);
        ::FreeLibrary(hWin);
        return false;
    }

    HANDLE hOut = ::CreateFileW(dst_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        err = "CreateFileW(dst) failed, err=" +
              std::to_string(::GetLastError());
        pClose(hUrl);
        pClose(hSession);
        ::FreeLibrary(hWin);
        return false;
    }

    char chunk[64 * 1024];
    DWORD got = 0;
    DWORD total = 0;
    bool io_ok = true;
    while (pRead(hUrl, chunk, sizeof(chunk), &got) && got > 0) {
        DWORD written = 0;
        if (!::WriteFile(hOut, chunk, got, &written, nullptr) || written != got) {
            err = "WriteFile failed, err=" + std::to_string(::GetLastError());
            io_ok = false;
            break;
        }
        total += got;
    }
    ::CloseHandle(hOut);
    pClose(hUrl);
    pClose(hSession);
    ::FreeLibrary(hWin);

    if (!io_ok) {
        ::DeleteFileW(dst_path.c_str());
        return false;
    }
    if (total == 0) {
        err = "downloaded 0 bytes";
        ::DeleteFileW(dst_path.c_str());
        return false;
    }
    err = "downloaded " + std::to_string(total) + " bytes";
    return true;
}

static void atu_install_vcredist_if_needed(const stub::ParsedPayload& p) {
    std::string flag = p.get("InstallVcRedist", "");
    if (flag.empty()) {
        atu::log_info("vcredist: InstallVcRedist not requested, skip");
        return;
    }
    bool enabled = (flag == "1" || flag == "yes" || flag == "true" ||
                    flag == "on");
    if (!enabled) {
        atu::log_info("vcredist: InstallVcRedist=" + flag + " (falsy), skip");
        return;
    }

    // Test/debug hook: VcRedistForceInstall=1 in the payload skips the registry
    // detection so E2E can exercise the download+execute path on a machine
    // that already ships the runtime. Never set this in a shipping build.
    bool force = (p.get("VcRedistForceInstall", "") == "1");
    std::string ver;
    bool installed = detect_vcredist_x64(ver);
    if (installed && !force) {
        atu::log_info("vcredist: VC++ 2015-2022 x64 already installed"
                      " (version=" + ver + "), skip bootstrapper");
        return;
    }
    if (force) {
        atu::log_warn("vcredist: VcRedistForceInstall=1 - skipping detection, "
                      "forcing bootstrapper run (test mode)");
    } else {
        atu::log_warn("vcredist: VC++ 2015-2022 x64 NOT detected on this system");
    }

    wchar_t tmp[MAX_PATH] = {0};
    if (!::GetTempPathW(MAX_PATH, tmp)) {
        atu::log_warn("vcredist: GetTempPathW failed - continuing without vcredist");
        return;
    }

    // Priority 1: local VcRedistSource embedded at compile time.
    // Use it directly, never touch the network.
    if (!p.vcredist_data.empty()) {
        atu::log_info("vcredist: using embedded VcRedistSource bootstrapper (" +
                      std::to_string(p.vcredist_data.size()) + " bytes)");
        std::wstring dst = std::wstring(tmp) + L"aptgeu_vcredist_x64.exe";
        HANDLE h = ::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            atu::log_warn("vcredist: CreateFileW(temp) failed, err=" +
                          std::to_string(::GetLastError()) +
                          " - continuing without vcredist");
            return;
        }
        DWORD written = 0;
        ::WriteFile(h, p.vcredist_data.data(),
                    static_cast<DWORD>(p.vcredist_data.size()), &written, nullptr);
        ::CloseHandle(h);
        atu::log_info("vcredist: extracted bootstrapper to temp (" +
                      std::to_string(written) + " bytes)");
        atu_run_vcredist_bootstrapper(dst);
        return;
    }

    // Priority 2: download from VcRedistURL.
    std::string url_u8 = p.get("VcRedistURL", "");
    if (url_u8.empty()) {
        atu::log_warn("vcredist: no embedded bootstrapper and no VcRedistURL; "
                      "cannot install, continuing without it");
        return;
    }
    std::wstring url = stub::utf8_to_wide(url_u8);
    std::wstring dst = std::wstring(tmp) + L"vc_redist.x64.exe";
    atu::log_info("vcredist: downloading bootstrapper from " + url_u8 +
                  " -> " + std::string(dst.begin(), dst.end()));
    std::string dlerr;
    if (!atu_download_file(url, dst, dlerr)) {
        atu::log_warn("vcredist: download FAILED: " + dlerr +
                      " - continuing without vcredist (install not blocked)");
        return;
    }
    atu::log_info(std::string("vcredist: download OK, ") + dlerr);
    atu_run_vcredist_bootstrapper(dst);
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct CliArgs {
    bool        silent     = false;   // /SILENT
    bool        very_silent = false;  // /VERYSILENT (implies silent + suppress popups)
    bool        uninstall  = false;   // /UNINSTALL
    bool        suppress   = false;   // /SUPPRESSMSGBOXES
    bool        no_restart = false;   // /NORESTART
    bool        close_apps = false;   // /CLOSEAPPLICATIONS
    bool        force_close_apps = false; // /FORCECLOSEAPPLICATIONS
    bool        restart_apps = false; // /RESTARTAPPLICATIONS (log-only for now)
    bool        no_icons   = false;   // /NOICONS
    int         restart_exit_code = 0; // /RESTARTEXITCODE=N
    bool        help       = false;   // /HELP or /?
    std::wstring dir_override;        // /DIR=...
    std::wstring log_path;            // /LOG=...
    std::wstring group_name;          // /GROUP=...
    std::string  lang_override;       // /LANG=zh
};

static CliArgs parse_cli() {
    CliArgs a;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return a;
    auto strip_quotes = [](std::wstring s) {
        while (!s.empty() && (s.front() == L'"' || s.front() == L' ')) s.erase(0, 1);
        while (!s.empty() && (s.back() == L'"' || s.back() == L' ')) s.pop_back();
        return s;
    };
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::wstring low; low.reserve(arg.size());
        for (wchar_t c : arg) low.push_back((wchar_t)towlower(c));

        if (low == L"/silent") a.silent = true;
        else if (low == L"/verysilent") { a.silent = true; a.very_silent = true; }
        else if (low == L"/uninstall") a.uninstall = true;
        else if (low == L"/suppressmsgboxes") a.suppress = true;
        else if (low == L"/norerestart" || low == L"/norestart") a.no_restart = true;
        else if (low == L"/closeapplications") a.close_apps = true;
        else if (low == L"/forcecloseapplications") { a.force_close_apps = true; a.close_apps = true; }
        else if (low == L"/restartapplications") a.restart_apps = true;
        else if (low == L"/noicons") a.no_icons = true;
        else if (low == L"/help" || low == L"/?") a.help = true;
        else if (low.rfind(L"/restartexitcode=", 0) == 0) {
            std::wstring v = arg.substr(17);
            try { a.restart_exit_code = std::stoi(v); } catch (...) { a.restart_exit_code = 0; }
        }
        else if (low.rfind(L"/dir=", 0) == 0) {
            a.dir_override = strip_quotes(arg.substr(5));
        } else if (low.rfind(L"/log=", 0) == 0) {
            a.log_path = strip_quotes(arg.substr(5));
        } else if (low.rfind(L"/group=", 0) == 0) {
            a.group_name = strip_quotes(arg.substr(7));
        } else if (low.rfind(L"/lang=", 0) == 0) {
            std::wstring wl = arg.substr(6);
            a.lang_override = std::string(wl.begin(), wl.end());
        }
    }
    ::LocalFree(argv);
    return a;
}

// ---------------------------------------------------------------------------
// Uninstall mode
// ---------------------------------------------------------------------------

static std::wstring dir_of_exe(const std::wstring& exe) {
    size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return exe.substr(0, slash);
}

static int run_uninstall(const CliArgs& cli) {
    atu::log_info("uninstall mode: begin");
    std::wstring self = [] {
        wchar_t buf[MAX_PATH * 2] = {0};
        DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
        return n ? std::wstring(buf, n) : std::wstring();
    }();
    std::wstring dir = dir_of_exe(self);
    std::wstring dat = dir + L"\\unins000.dat";

    // Pull RegKeyName out of embedded payload (best effort).
    std::string reg_key_utf8;
    {
        stub::ParsedPayload p; std::string err;
        if (stub::read_self_payload(p, err)) {
            reg_key_utf8 = p.get("RegKeyName", "ApTgeuApp");
        } else {
            reg_key_utf8 = "ApTgeuApp";
        }
    }
    std::wstring reg_key = stub::utf8_to_wide(reg_key_utf8);

    if (!cli.silent) {
        int r = ::MessageBoxW(nullptr,
                             L"确定要卸载此应用程序吗？",
                             L"ApTgeu 卸载",
                             MB_YESNO | MB_ICONQUESTION);
        if (r != IDYES) return 0;
    }

    // Create a system restore point before uninstalling (best-effort, non-fatal).
    atu_create_restore_point(L"ApTgeu: before uninstall", /*is_uninstall=*/true);

    // Delete listed files.
    int ndel = stub::uninstall_delete_listed_files(dat);
    atu::log_info("uninstall: deleted listed files, count=" + std::to_string(ndel));

    // v0.23: delete user-declared [Registry] entries.
    {
        stub::ParsedPayload p; std::string err2;
        if (stub::read_self_payload(p, err2)) {
            stub::unapply_registry_entries(p, dir);
            atu::log_info("uninstall: [Registry] entries removed");
            // v0.24: delete user-declared [INI] entries (uninsdeleteentry).
            stub::unapply_ini_entries(p, dir);
            atu::log_info("uninstall: [INI] entries removed");
        }
    }

    // Delete registry uninstall key.
    stub::delete_uninstall_registry(reg_key);
    atu::log_info("uninstall: registry key removed");

    // Remove the whole install dir (best effort; running exe will be skipped).
    stub::uninstall_remove_tree(dir);

    // Schedule self-delete via detached cmd.
    stub::schedule_self_delete(self, dir);
    atu::log_info("uninstall: done, self-delete scheduled");
    atu::log_shutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// GUI install
// ---------------------------------------------------------------------------

static int run_gui_install(const stub::ParsedPayload& payload,
                           bool no_icons,
                           const std::wstring& group_name) {
    const stub::UiTheme& theme = payload.theme;

    auto fail = [](const wchar_t* msg) -> int {
        ::MessageBoxW(nullptr, msg, L"ApTgeu Setup", MB_OK | MB_ICONERROR);
        return 1;
    };

    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, WndProc, 0L, 0L,
                       ::GetModuleHandle(nullptr), nullptr,
                       ::LoadCursor(nullptr, IDC_ARROW),
                       (HBRUSH)(COLOR_WINDOW + 1),
                       nullptr, L"ApTgeuSetupWnd", nullptr };
    if (!::RegisterClassExW(&wc)) return fail(L"RegisterClassExW failed");

    int base_w = theme.window_width  > 0 ? theme.window_width  : 800;
    int base_h = theme.window_height > 0 ? theme.window_height : 500;
    int win_w = (int)(base_w * scale);
    int win_h = (int)(base_h * scale);
    int sx = (::GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
    int sy = (::GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

    std::wstring title_w = theme.title.empty()
        ? L"ApTgeu Setup"
        : stub::utf8_to_wide(theme.title);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (theme.resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, title_w.c_str(),
                                 style, sx, sy, win_w, win_h,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return fail(L"CreateWindowW failed");

    // --- GL context ---
    if (!CreateDeviceWGL(hwnd, &g_MainWindow)) {
        CleanupDeviceWGL(hwnd, &g_MainWindow);
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return fail(L"CreateDeviceWGL failed (pixel format / OpenGL context)");
    }
    if (!::wglMakeCurrent(g_MainWindow.hDC, g_hRC)) {
        return fail(L"wglMakeCurrent failed");
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // --- ImGui context ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Build font atlas BEFORE backend inits. Load system Chinese font so
    // CJK glyphs render (default ImGui embedded font has no CJK).
    // Do NOT call Clear() -- keep the embedded default as fallback.
    {
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font) {
            io.FontDefault = font;
        } else {
            font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f);
            if (font) io.FontDefault = font;
        }
        // If both failed, the embedded default font is still in the atlas.
    }

    // Build theme + font atlas BEFORE backend inits, so the GL3 backend
    // picks up the correct font texture on first NewFrame.
    stub::WizardState state;
    state.payload = &payload;
    // v0.21: propagate /NOICONS and /GROUP= into the GUI wizard finalize step.
    state.no_icons = no_icons;
    state.group_name = group_name;
    std::string def = payload.get("DefaultDirName", "C:\\ApTgeuApp");
    strncpy(state.install_dir_buf, def.c_str(), sizeof(state.install_dir_buf) - 1);

    // v0.20: WizardResizable overrides the theme's resizable flag.
    {
        std::string wr = payload.get("WizardResizable", "");
        for (auto& c : wr) c = (char)tolower((unsigned char)c);
        if (wr == "1" || wr == "yes" || wr == "true" || wr == "on") {
            const_cast<stub::UiTheme&>(theme).resizable = 1;
        } else if (wr == "0" || wr == "no" || wr == "false" || wr == "off") {
            const_cast<stub::UiTheme&>(theme).resizable = 0;
        }
    }

    // v0.20: honor DisableWelcomePage / DisableFinishedPage etc.
    stub::wizard_apply_page_flags(&state);

    stub::wizard_apply_theme(&state);

    // --- Backends ---
    if (!ImGui_ImplWin32_InitForOpenGL(hwnd)) {
        return fail(L"ImGui_ImplWin32_InitForOpenGL failed");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        return fail(L"ImGui_ImplOpenGL3_Init failed (OpenGL 3.0+ required)");
    }

    // Load PNG textures into GL (needs current GL context; safe after backend init).
    stub::wizard_init_theme_textures(&state);

    // Clear color from theme.
    ImVec4 bg = ImVec4(theme.background[0]/255.f, theme.background[1]/255.f,
                       theme.background[2]/255.f, theme.background[3]/255.f);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (::IsIconic(hwnd)) { ::Sleep(10); continue; }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        stub::wizard_render(&state);

        if (state.request_exit) ::PostQuitMessage(0);

        ImGui::Render();
        ImGui::EndFrame();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(bg.x, bg.y, bg.z, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(g_MainWindow.hDC);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceWGL(hwnd, &g_MainWindow);
    ::wglDeleteContext(g_hRC);
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    CliArgs cli = parse_cli();

    // v0.21: /VERYSILENT implies /SUPPRESSMSGBOXES (no popups at all).
    if (cli.very_silent) cli.suppress = true;

    // v0.20: open log file. Empty path -> default %TEMP%\aptgeu_setup.log.
    // /LOG="..." overrides the default location (quotes stripped in parser).
    atu::log_init(cli.log_path);
    atu::log_info("=== aptgeu setup.exe start ===");
    atu::log_info(std::string("cli: silent=") + (cli.silent?"1":"0") +
                  " verysilent=" + (cli.very_silent?"1":"0") +
                  " uninstall=" + (cli.uninstall?"1":"0") +
                  " norestart=" + (cli.no_restart?"1":"0") +
                  " closeapps=" + (cli.close_apps?"1":"0") +
                  " forceclose=" + (cli.force_close_apps?"1":"0") +
                  " restartapps=" + (cli.restart_apps?"1":"0") +
                  " noicons=" + (cli.no_icons?"1":"0") +
                  " restartexitcode=" + std::to_string(cli.restart_exit_code));
    if (!cli.group_name.empty()) {
        atu::log_info(std::string("cli: group=") +
                      std::string(cli.group_name.begin(), cli.group_name.end()));
    }
    if (!cli.log_path.empty()) {
        atu::log_info(std::string("cli: custom log=") +
                      std::string(cli.log_path.begin(), cli.log_path.end()));
    }

    if (cli.help) {
        ::MessageBoxW(nullptr,
            L"ApTgeu Setup\n\n"
            L"Usage: setup.exe [options]\n"
            L"  /SILENT        Quiet install\n"
            L"  /VERYSILENT    Quiet install, no popups at all\n"
            L"  /DIR=path      Install directory\n"
            L"  /LOG=path      Write log\n"
            L"  /SUPPRESSMSGBOXES  No error popups\n"
            L"  /CLOSEAPPLICATIONS Close running app\n"
            L"  /FORCECLOSEAPPLICATIONS  Kill app without prompting\n"
            L"  /RESTARTAPPLICATIONS  Restart closed apps after install\n"
            L"  /NORESTART     Don't reboot\n"
            L"  /RESTARTEXITCODE=N  Exit code N on reboot-needed\n"
            L"  /GROUP=name    Start Menu folder name\n"
            L"  /NOICONS       Don't create shortcuts\n"
            L"  /? /HELP       Show this help",
            L"ApTgeu Setup", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // B6: single-instance mutex.
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"ApTgeu_Setup_FileTgie");
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!cli.suppress)
            ::MessageBoxW(nullptr, L"安装程序已在运行", L"ApTgeu Setup", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (cli.uninstall) {
        return run_uninstall(cli);
    }

    // Installer path: need payload.
    // v0.16: set XOR password from /PASSWORD=
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                std::wstring a = argv[i];
                if (a.size() > 10 && _wcsnicmp(a.c_str(), L"/PASSWORD=", 10) == 0) {
                    std::wstring pw = a.substr(10);
                    stub::g_password = std::string(pw.begin(), pw.end());
                    break;
                }
            }
            ::LocalFree(argv);
        }
    }
    stub::ParsedPayload payload;
    std::string err;
    if (!stub::read_self_payload(payload, err)) {
        atu::log_error(std::string("read_self_payload failed: ") + err);
        // v0.2.5: encrypted payload with missing/wrong password -> "密码错误".
        // v0.16.1: in silent mode (or when /SUPPRESSMSGBOXES) do not pop a modal
        //   MessageBox; the process must exit(1) immediately so automation can
        //   observe the failure. This matters for DiskSliceSize missing-slice
        //   errors, which otherwise leave setup.exe hung on a dialog.
        bool quiet = cli.silent || cli.very_silent || cli.suppress;
        if (!quiet) {
            if (stub::g_payload_tampered) {
                ::MessageBoxW(nullptr, L"数据被篡改", L"ApTgeu Setup",
                              MB_OK | MB_ICONERROR);
            } else if (stub::g_payload_encrypted) {
                ::MessageBoxW(nullptr, L"密码错误", L"ApTgeu Setup",
                              MB_OK | MB_ICONERROR);
            } else {
                std::wstring wmsg = L"ApTgeu stub: payload not found\n\n";
                wmsg += stub::utf8_to_wide(err);
                ::MessageBoxW(nullptr, wmsg.c_str(), L"ApTgeu Setup",
                              MB_OK | MB_ICONERROR);
            }
        }
        atu::log_shutdown();
        return 1;
    }
    atu::log_info("install: payload read OK, app=" + payload.get("AppName","?") +
                  " ver=" + payload.get("AppVersion","?"));

    // v0.5 P2: set UI language from payload config.
    atu::i18n_set_language(cli.lang_override.empty() ? payload.get("Language", "zh") : cli.lang_override);
    // v0.16: apply Messages.* overrides from payload config
    for (const auto& [k, v] : payload.config) {
        if (k.size() > 8 && k.substr(0, 8) == "Messages") {
            atu::i18n_override(k.c_str() + 8, v);
        }
    }

    // v0.22: if [Setup] InstallVcRedist=yes was set, detect VC++ 2015-2022 x64
    // and silently install it before we touch the app's files.
    atu_install_vcredist_if_needed(payload);

    // Create a system restore point at install start (best-effort, non-fatal).
    {
        std::wstring rp_desc = L"ApTgeu: before install - ";
        rp_desc += stub::utf8_to_wide(payload.get("AppName", "ApTgeu App"));
        atu_create_restore_point(rp_desc.c_str(), /*is_uninstall=*/false);
    }

    // v0.15: check for existing version and uninstall it first
    {
        std::wstring reg_key = stub::utf8_to_wide(payload.get("RegKeyName", "ApTgeuApp"));
        std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + reg_key;
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t ver[256] = {0}; DWORD sz = sizeof(ver);
            RegQueryValueExW(hKey, L"DisplayVersion", nullptr, nullptr, (LPBYTE)ver, &sz);
            wchar_t unins_loc[MAX_PATH] = {0}; DWORD usz = sizeof(unins_loc);
            RegQueryValueExW(hKey, L"UninstallString", nullptr, nullptr, (LPBYTE)unins_loc, &usz);
            RegCloseKey(hKey);
            if (ver[0] != 0) {
                // old version found
                std::wstring msg = L"检测到旧版本: ";
                msg += ver;
                msg += L"\n是否先卸载旧版本?";
                if (cli.silent) {
                    // auto uninstall silently: copy unins to temp first to avoid
                    // deadlock when installing to same dir
                    if (unins_loc[0] != 0) {
                        std::wstring ucmd = unins_loc;
                        size_t q2 = ucmd.find(L'"', 1);
                        if (ucmd[0] == L'"' && q2 != std::wstring::npos) {
                            ucmd = ucmd.substr(1, q2 - 1);
                        }
                        // copy to temp
                        wchar_t temp_dir[MAX_PATH];
                        GetTempPathW(MAX_PATH, temp_dir);
                        std::wstring temp_unins = std::wstring(temp_dir) + L"\\aptgeu_old_unins.exe";
                        CopyFileW(ucmd.c_str(), temp_unins.c_str(), FALSE);
                        SHELLEXECUTEINFOW sei = {sizeof(sei)};
                        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                        sei.lpVerb = L"open";
                        sei.lpFile = temp_unins.c_str();
                        sei.lpParameters = L"/SILENT";
                        sei.nShow = SW_HIDE;
                        ShellExecuteExW(&sei);
                        if (sei.hProcess) {
                            WaitForSingleObject(sei.hProcess, 15000);
                            CloseHandle(sei.hProcess);
                        }
                        DeleteFileW(temp_unins.c_str());
                    }
                } else {
                    int r = MessageBoxW(nullptr, msg.c_str(), L"ApTgeu Setup", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES && unins_loc[0] != 0) {
                        std::wstring ucmd = unins_loc;
                        size_t q2 = ucmd.find(L'"', 1);
                        if (ucmd[0] == L'"' && q2 != std::wstring::npos) {
                            ucmd = ucmd.substr(1, q2 - 1);
                        }
                        wchar_t temp_dir[MAX_PATH];
                        GetTempPathW(MAX_PATH, temp_dir);
                        std::wstring temp_unins = std::wstring(temp_dir) + L"\\aptgeu_old_unins.exe";
                        CopyFileW(ucmd.c_str(), temp_unins.c_str(), FALSE);
                        SHELLEXECUTEINFOW sei = {sizeof(sei)};
                        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                        sei.lpVerb = L"open";
                        sei.lpFile = temp_unins.c_str();
                        sei.lpParameters = L"/SILENT";
                        sei.nShow = SW_HIDE;
                        ShellExecuteExW(&sei);
                        if (sei.hProcess) {
                            WaitForSingleObject(sei.hProcess, 15000);
                            CloseHandle(sei.hProcess);
                        }
                        DeleteFileW(temp_unins.c_str());
                    }
                }
            }
        }
    }

    if (cli.silent) {
        std::wstring dir;
        if (!cli.dir_override.empty()) {
            dir = cli.dir_override;
        } else {
            dir = stub::utf8_to_wide(payload.get("DefaultDirName", "C:\\ApTgeuApp"));
        }
        atu::log_info(std::string("install: silent mode, dir=") +
                      std::string(dir.begin(), dir.end()));
        stub::SilentInstallOpts opts;
        opts.no_icons        = cli.no_icons;
        opts.force_close_apps = cli.force_close_apps;
        opts.close_apps      = cli.close_apps;
        opts.group_name      = cli.group_name;
        std::string e;
        if (!stub::wizard_run_silent_install(payload, dir, e, opts)) {
            atu::log_error(std::string("install: silent install FAILED: ") + e);
            if (!cli.very_silent && !cli.suppress)
                ::MessageBoxW(nullptr, L"静默安装失败", L"ApTgeu Setup", MB_OK | MB_ICONERROR);
            atu::log_shutdown();
            return 1;
        }
        atu::log_info("install: silent install OK");
        atu::log_shutdown();
        // /RESTARTEXITCODE=N: return N on successful install (default 0).
        return cli.restart_exit_code;
    }

    int rc = run_gui_install(payload, cli.no_icons, cli.group_name);
    if (rc == 0) {
        // v0.2.4: post-install update check (GUI only; silent path returns earlier).
        atu_check_for_updates(payload.get("UpdateURL", ""),
                              payload.get("AppVersion", "0.0.0"));
    }
    atu::log_info("install: GUI install returned " + std::to_string(rc));
    atu::log_shutdown();
    return rc;
}

// ---------------------------------------------------------------------------
// WGL helpers (copied from imgui example_win32_opengl3)
// ---------------------------------------------------------------------------

bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    HDC hDc = ::GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;

    int pf = ::ChoosePixelFormat(hDc, &pfd);
    if (pf == 0) return false;
    if (::SetPixelFormat(hDc, pf, &pfd) == FALSE) return false;
    ::ReleaseDC(hWnd, hDc);

    data->hDC = ::GetDC(hWnd);
    if (g_hRC) return true;

    HGLRC tempRC = ::wglCreateContext(data->hDC);
    if (!tempRC) { ::ReleaseDC(hWnd, data->hDC); return false; }
    if (!::wglMakeCurrent(data->hDC, tempRC)) {
        ::wglDeleteContext(tempRC);
        ::ReleaseDC(hWnd, data->hDC);
        return false;
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(0x821B, &major); // GL_MAJOR_VERSION
    glGetIntegerv(0x821C, &minor); // GL_MINOR_VERSION
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (major == 0 && minor == 0) {
        sscanf(ver, "%d.%d", &major, &minor);
    }
    GLuint gl_ver = (GLuint)(major * 100 + minor * 10);

    g_hRC = tempRC;
    if (gl_ver >= 300) return true;

    typedef HGLRC (WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    auto wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)::wglGetProcAddress("wglCreateContextAttribsARB");

    const int attribs[] = {
        0x2091, 3,       // WGL_CONTEXT_MAJOR_VERSION_ARB
        0x2092, 0,       // WGL_CONTEXT_MINOR_VERSION_ARB
        0x9126, 0x0001,  // WGL_CONTEXT_PROFILE_MASK_ARB / CORE
        0
    };
    HGLRC newRC = nullptr;
    if (wglCreateContextAttribsARB)
        newRC = wglCreateContextAttribsARB(data->hDC, 0, attribs);
    if (newRC) {
        ::wglMakeCurrent(nullptr, nullptr);
        ::wglDeleteContext(tempRC);
        g_hRC = newRC;
        ::wglMakeCurrent(data->hDC, g_hRC);
    }
    return true;
}

void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    ::wglMakeCurrent(nullptr, nullptr);
    ::ReleaseDC(hWnd, data->hDC);
}
