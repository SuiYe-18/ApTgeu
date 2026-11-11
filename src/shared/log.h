/* v0.20: file + stdout logging, default %TEMP%\aptgeu_setup.log */
#pragma once
#include <string>

namespace atu {

enum LogLevel { LOG_TRACE=0, LOG_DEBUG=1, LOG_INFO=2, LOG_WARN=3, LOG_ERROR=4 };

// Open the log file. If `path` is empty, defaults to %TEMP%\aptgeu_setup.log.
// Safe to call multiple times; re-opening with a new path switches target.
void log_init(const std::wstring& path = L"");
void log_write(LogLevel lvl, const char* file, int line, const std::string& msg);
void log_shutdown();

// Convenience: log a plain message at INFO/WARN/ERROR (no file/line).
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

} // namespace atu

#define ATU_LOG_TRACE(msg) ::atu::log_write(::atu::LOG_TRACE, __FILE__, __LINE__, msg)
#define ATU_LOG_DEBUG(msg) ::atu::log_write(::atu::LOG_DEBUG, __FILE__, __LINE__, msg)
#define ATU_LOG_INFO(msg)  ::atu::log_write(::atu::LOG_INFO,  __FILE__, __LINE__, msg)
#define ATU_LOG_WARN(msg)  ::atu::log_write(::atu::LOG_WARN,  __FILE__, __LINE__, msg)
#define ATU_LOG_ERROR(msg) ::atu::log_write(::atu::LOG_ERROR, __FILE__, __LINE__, msg)
