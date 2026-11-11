#include "log.h"

// log.cpp is only linked into the Windows installer stub.
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>
#include <cstdio>
#include <ctime>

namespace atu {

static HANDLE  g_handle = INVALID_HANDLE_VALUE;
static std::mutex g_mu;
static LogLevel g_min = LOG_TRACE;   // v0.20: log everything by default
static std::wstring g_current_path;

static std::wstring default_log_path() {
    wchar_t tmp[MAX_PATH] = {0};
    DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return L"aptgeu_setup.log";
    return std::wstring(tmp) + L"aptgeu_setup.log";
}

void log_init(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_handle);
        g_handle = INVALID_HANDLE_VALUE;
    }
    std::wstring p = path.empty() ? default_log_path() : path;
    g_current_path = p;
    // OPEN_ALWAYS + FILE_APPEND_DATA = create if missing, append if exists.
    g_handle = ::CreateFileW(p.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static std::string now_timestamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void log_write(LogLevel lvl, const char* /*file*/, int /*line*/, const std::string& msg) {
    if (lvl < g_min) return;
    const char* lvl_s[] = {"TRACE","DEBUG","INFO","WARN","ERROR"};
    std::string line_out = "[" + now_timestamp() + "] [" + lvl_s[lvl] + "] " + msg + "\n";
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_handle != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            ::WriteFile(g_handle, line_out.data(), (DWORD)line_out.size(), &w, nullptr);
        }
    }
    std::fwrite(line_out.c_str(), 1, line_out.size(), stderr);
}

void log_info(const std::string& msg)  { log_write(LOG_INFO,  nullptr, 0, msg); }
void log_warn(const std::string& msg)  { log_write(LOG_WARN,  nullptr, 0, msg); }
void log_error(const std::string& msg) { log_write(LOG_ERROR, nullptr, 0, msg); }

void log_shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_handle);
        g_handle = INVALID_HANDLE_VALUE;
    }
}

} // namespace atu
