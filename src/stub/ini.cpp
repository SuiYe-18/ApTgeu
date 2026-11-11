/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "ini.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <vector>

#include "log.h"
#include "unpack.h"

namespace stub {

static std::wstring utf8_to_ws(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static std::string ws_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          s.data(), n, nullptr, nullptr);
    return s;
}

// Replace every {app} occurrence in `s` with install_dir (wide).
static std::wstring substitute_app(const std::wstring& s,
                                   const std::wstring& install_dir) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 5 <= s.size() && s[i] == L'{' && s[i+1] == L'a' && s[i+2] == L'p'
            && s[i+3] == L'p' && s[i+4] == L'}') {
            out += install_dir;
            i += 5;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

static bool contains_ci(const std::string& haystack, const std::string& needle) {
    std::string h, n;
    h.reserve(haystack.size());
    n.reserve(needle.size());
    for (char c : haystack) h.push_back((char)tolower((unsigned char)c));
    for (char c : needle)   n.push_back((char)tolower((unsigned char)c));
    return h.find(n) != std::string::npos;
}

bool parse_ini_entry(const std::string& line_utf8,
                     const std::wstring& install_dir,
                     IniSpec& out) {
    // Split on '|' into up to 5 fields: filename|section|key|value|flags
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line_utf8) {
        if (c == '|') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    // filename, section, key are required; value/flags may be empty.
    if (parts.size() < 3) return false;

    out.filename = utf8_to_ws(parts[0]);
    out.section  = utf8_to_ws(parts[1]);
    out.key      = utf8_to_ws(parts[2]);
    out.value    = (parts.size() >= 4) ? utf8_to_ws(parts[3]) : std::wstring();
    std::string flags = (parts.size() >= 5) ? parts[4] : std::string();
    out.unins_delete_entry    = contains_ci(flags, "uninsdeleteentry");
    out.create_if_doesnt_exist = contains_ci(flags, "createifdoesntexist");

    if (out.filename.empty()) return false;
    if (out.section.empty())  return false;
    if (out.key.empty())      return false;

    // Substitute {app} in filename.
    out.filename = substitute_app(out.filename, install_dir);
    return true;
}

bool write_ini_entry(const IniSpec& spec) {
    // Honor createifdoesntexist: if the flag is NOT set and the file does
    // not exist, skip writing (Inno Setup semantics).
    if (!spec.create_if_doesnt_exist) {
        DWORD attr = ::GetFileAttributesW(spec.filename.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES ||
            (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            atu::log_info("ini: file does not exist and createifdoesntexist "
                          "not set, skipping: " +
                          ws_to_utf8(spec.filename));
            return true;
        }
    }

    BOOL ok = ::WritePrivateProfileStringW(
        spec.section.c_str(),
        spec.key.c_str(),
        spec.value.c_str(),
        spec.filename.c_str());
    if (!ok) {
        atu::log_error("ini: WritePrivateProfileStringW failed, err=" +
                       std::to_string(::GetLastError()) +
                       " file=" + ws_to_utf8(spec.filename) +
                       " section=" + ws_to_utf8(spec.section) +
                       " key=" + ws_to_utf8(spec.key));
        return false;
    }
    atu::log_info("ini: wrote [" + ws_to_utf8(spec.section) + "] " +
                  ws_to_utf8(spec.key) + "=" + ws_to_utf8(spec.value) +
                  " -> " + ws_to_utf8(spec.filename));
    return true;
}

bool delete_ini_entry(const IniSpec& spec) {
    // Per MSDN: if lpString is NULL, the key pointed to by lpKeyName is
    // deleted from the specified initialization file.
    BOOL ok = ::WritePrivateProfileStringW(
        spec.section.c_str(),
        spec.key.c_str(),
        nullptr,
        spec.filename.c_str());
    if (!ok) {
        DWORD e = ::GetLastError();
        // ERROR_FILE_NOT_FOUND (2) / ERROR_PATH_NOT_FOUND (3) are OK - the
        // file was already removed by the uninstaller's tree deletion.
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
            atu::log_warn("ini: delete WritePrivateProfileStringW failed, err=" +
                          std::to_string(e) +
                          " file=" + ws_to_utf8(spec.filename));
            return false;
        }
    }
    atu::log_info("ini: deleted [" + ws_to_utf8(spec.section) + "] " +
                  ws_to_utf8(spec.key) + " from " + ws_to_utf8(spec.filename));
    return true;
}

bool apply_ini_entries(const ParsedPayload& payload,
                       const std::wstring& install_dir,
                       std::string& out_unins_lines) {
    int count = 0;
    try { count = std::stoi(payload.get("IniCount", "0")); } catch (...) { count = 0; }
    if (count <= 0) return true;

    bool all_ok = true;
    for (int i = 0; i < count; ++i) {
        std::string line = payload.get("Ini" + std::to_string(i), "");
        if (line.empty()) continue;
        IniSpec spec;
        if (!parse_ini_entry(line, install_dir, spec)) {
            atu::log_warn("ini: cannot parse Ini" + std::to_string(i) +
                          " line='" + line + "'");
            continue;
        }
        if (!write_ini_entry(spec)) {
            all_ok = false;
        }
        // Persist for the standalone uninstaller. Re-emit in UTF-8 with {app}
        // already substituted.
        out_unins_lines += ws_to_utf8(spec.filename);
        out_unins_lines += "|";
        out_unins_lines += ws_to_utf8(spec.section);
        out_unins_lines += "|";
        out_unins_lines += ws_to_utf8(spec.key);
        out_unins_lines += "|";
        out_unins_lines += ws_to_utf8(spec.value);
        out_unins_lines += "|";
        out_unins_lines += spec.unins_delete_entry ? "1" : "0";
        out_unins_lines += "\n";
    }
    return all_ok;
}

void unapply_ini_entries(const ParsedPayload& payload,
                         const std::wstring& install_dir) {
    int count = 0;
    try { count = std::stoi(payload.get("IniCount", "0")); } catch (...) { count = 0; }
    for (int i = 0; i < count; ++i) {
        std::string line = payload.get("Ini" + std::to_string(i), "");
        if (line.empty()) continue;
        IniSpec spec;
        if (!parse_ini_entry(line, install_dir, spec)) continue;
        if (spec.unins_delete_entry) {
            delete_ini_entry(spec);
        }
    }
}

} // namespace stub
