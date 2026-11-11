/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "registry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <vector>

#include "log.h"
#include "unpack.h"

namespace stub {

static const wchar_t* kUninstallRoot =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

static std::wstring subkey(const std::wstring& reg_key_name) {
    return std::wstring(kUninstallRoot) + reg_key_name;
}

static bool set_string(HKEY k, const wchar_t* name, const std::wstring& v) {
    return ::RegSetValueExW(k, name, 0, REG_SZ,
                            (const BYTE*)v.c_str(),
                            (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static bool set_dword(HKEY k, const wchar_t* name, DWORD v) {
    return ::RegSetValueExW(k, name, 0, REG_DWORD,
                            (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
}

bool write_uninstall_registry(const std::wstring& reg_key_name,
                              const std::wstring& display_name,
                              const std::wstring& display_version,
                              const std::wstring& publisher,
                              const std::wstring& install_location,
                              const std::wstring& uninstall_string,
                              const std::wstring& display_icon) {
    std::wstring path = subkey(reg_key_name);
    HKEY hKey = nullptr;
    LSTATUS st = ::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                   0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                                   nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS) return false;

    bool ok = true;
    ok &= set_string(hKey, L"DisplayName",      display_name);
    ok &= set_string(hKey, L"DisplayVersion",  display_version);
    ok &= set_string(hKey, L"Publisher",       publisher);
    ok &= set_string(hKey, L"InstallLocation", install_location);
    ok &= set_string(hKey, L"UninstallString", uninstall_string);
    ok &= set_dword (hKey, L"NoModify",        1);
    ok &= set_dword (hKey, L"NoRepair",       1);
    if (!display_icon.empty()) {
        ok &= set_string(hKey, L"DisplayIcon", display_icon);
    }

    ::RegCloseKey(hKey);
    return ok;
}

bool delete_uninstall_registry(const std::wstring& reg_key_name) {
    std::wstring path = subkey(reg_key_name);
    // RegDeleteKeyExW recursively deletes subkey + values on Windows.
    LSTATUS st = ::RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(),
                                   KEY_WOW64_64KEY, 0);
    return st == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// v0.23: generic [Registry] entries
// ---------------------------------------------------------------------------

static HKEY root_handle(const std::wstring& root) {
    std::wstring r = root;
    for (auto& c : r) c = (wchar_t)towlower(c);
    if (r == L"hkcu" || r == L"hkey_current_user") return HKEY_CURRENT_USER;
    if (r == L"hklm" || r == L"hkey_local_machine") return HKEY_LOCAL_MACHINE;
    if (r == L"hkcr" || r == L"hkey_classes_root")  return HKEY_CLASSES_ROOT;
    return nullptr;
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

static std::wstring utf8_to_ws(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

bool parse_reg_entry(const std::string& line_utf8,
                     const std::wstring& install_dir,
                     RegValueSpec& out) {
    // Split on '|' into up to 6 fields.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line_utf8) {
        if (c == '|') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    // Expected: root | subkey | valuetype | valuename | valuedata | uninsdeletekey
    if (parts.size() < 5) return false;

    out.root   = utf8_to_ws(parts[0]);
    out.subkey = utf8_to_ws(parts[1]);
    out.value_type = utf8_to_ws(parts[2]);
    out.value_name = utf8_to_ws(parts[3]);
    out.value_data = utf8_to_ws(parts[4]);
    out.unins_delete_key = false;
    if (parts.size() >= 6) {
        std::string f = parts[5];
        for (auto& c : f) c = (char)tolower((unsigned char)c);
        out.unins_delete_key = (f == "1" || f == "yes" || f == "true");
    }

    if (root_handle(out.root) == nullptr) return false;
    if (out.subkey.empty()) return false;

    // Substitute {app} in subkey / valuename / valuedata.
    out.subkey     = substitute_app(out.subkey, install_dir);
    out.value_name = substitute_app(out.value_name, install_dir);
    out.value_data = substitute_app(out.value_data, install_dir);
    return true;
}

bool write_reg_entry(const RegValueSpec& spec) {
    HKEY root = root_handle(spec.root);
    if (!root) {
        atu::log_warn("registry: unknown root '" +
                      std::string(spec.root.begin(), spec.root.end()) + "'");
        return false;
    }

    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LSTATUS st = ::RegCreateKeyExW(root, spec.subkey.c_str(), 0, nullptr,
                                   REG_OPTION_NON_VOLATILE,
                                   KEY_SET_VALUE | KEY_WOW64_64KEY,
                                   nullptr, &hKey, &disposition);
    if (st != ERROR_SUCCESS) {
        atu::log_error("registry: RegCreateKeyExW failed, rc=" +
                       std::to_string(st) + " subkey=" +
                       std::string(spec.subkey.begin(), spec.subkey.end()));
        return false;
    }

    bool ok = false;
    std::wstring vt = spec.value_type;
    for (auto& c : vt) c = (wchar_t)towlower(c);
    if (vt == L"dword" || vt == L"d" || vt == L"integer") {
        DWORD v = 0;
        // ValueData is decimal (default) or "0x..." hex.
        std::wstring d = spec.value_data;
        if (d.size() >= 2 && (d[0] == L'0') && (d[1] == L'x' || d[1] == L'X')) {
            v = (DWORD)wcstoul(d.c_str() + 2, nullptr, 16);
        } else {
            v = (DWORD)wcstoul(d.c_str(), nullptr, 10);
        }
        ok = (::RegSetValueExW(hKey,
                               spec.value_name.empty() ? nullptr : spec.value_name.c_str(),
                               0, REG_DWORD,
                               (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS);
    } else {
        // Default: REG_SZ
        ok = (::RegSetValueExW(hKey,
                               spec.value_name.empty() ? nullptr : spec.value_name.c_str(),
                               0, REG_SZ,
                               (const BYTE*)spec.value_data.c_str(),
                               (DWORD)((spec.value_data.size() + 1) * sizeof(wchar_t)))
              == ERROR_SUCCESS);
    }

    ::RegCloseKey(hKey);
    if (!ok) {
        atu::log_error("registry: RegSetValueExW failed for value '" +
                       std::string(spec.value_name.begin(), spec.value_name.end()) + "'");
    }
    return ok;
}

bool delete_reg_entry(const RegValueSpec& spec) {
    HKEY root = root_handle(spec.root);
    if (!root) return false;

    bool any_ok = true;
    if (spec.unins_delete_key) {
        // Recursively delete the whole key (and all subkeys).
        // RegDeleteKeyExW on Vista+ does NOT recurse - must enumerate subkeys.
        // Use a small recursive helper.
        std::function<bool(HKEY, const std::wstring&)> del_tree;
        del_tree = [&](HKEY r, const std::wstring& path) -> bool {
            bool all_ok = true;
            HKEY h;
            if (::RegOpenKeyExW(r, path.c_str(), 0,
                                KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &h)
                == ERROR_SUCCESS) {
                wchar_t subname[256];
                DWORD idx = 0;
                DWORD subname_len = _countof(subname);
                while (::RegEnumKeyExW(h, idx, subname, &subname_len,
                                       nullptr, nullptr, nullptr, nullptr)
                       == ERROR_SUCCESS) {
                    std::wstring child = path + L"\\" + subname;
                    if (!del_tree(r, child)) all_ok = false;
                    // after recursion, the child should be empty now; try next
                    // without incrementing idx (same index works after deletion).
                }
                ::RegCloseKey(h);
            }
            LSTATUS st = ::RegDeleteKeyExW(r, path.c_str(), KEY_WOW64_64KEY, 0);
            if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
                atu::log_warn("registry: RegDeleteKeyExW failed, rc=" +
                              std::to_string(st) + " path=" +
                              std::string(path.begin(), path.end()));
                all_ok = false;
            }
            return all_ok;
        };
        any_ok = del_tree(root, spec.subkey);
    } else {
        // Delete only the named value.
        HKEY h;
        LSTATUS st = ::RegOpenKeyExW(root, spec.subkey.c_str(), 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, &h);
        if (st == ERROR_SUCCESS) {
            st = ::RegDeleteValueW(h,
                                   spec.value_name.empty() ? nullptr
                                                          : spec.value_name.c_str());
            ::RegCloseKey(h);
        }
        // ERROR_FILE_NOT_FOUND / ERROR_BADKEY / ERROR_PATH_NOT_FOUND are OK
        // (already gone).
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND &&
            st != ERROR_PATH_NOT_FOUND) {
            atu::log_warn("registry: RegDeleteValueW failed, rc=" +
                          std::to_string(st) + " name=" +
                          std::string(spec.value_name.begin(), spec.value_name.end()));
            any_ok = false;
        }
    }
    return any_ok;
}

bool apply_registry_entries(const ParsedPayload& payload,
                            const std::wstring& install_dir,
                            std::string& out_unins_lines) {
    int count = 0;
    try { count = std::stoi(payload.get("RegCount", "0")); } catch (...) { count = 0; }
    if (count <= 0) return true;

    bool all_ok = true;
    for (int i = 0; i < count; ++i) {
        std::string line = payload.get("Reg" + std::to_string(i), "");
        if (line.empty()) continue;
        RegValueSpec spec;
        if (!parse_reg_entry(line, install_dir, spec)) {
            atu::log_warn("registry: cannot parse Reg" + std::to_string(i) +
                          " line='" + line + "'");
            continue;
        }
        if (!write_reg_entry(spec)) {
            all_ok = false;
        }
        // Persist for uninstaller. Re-emit in UTF-8 with {app} already
        // substituted (the uninstaller knows g_install_dir, but we persist
        // the absolute path so it doesn't need to re-substitute).
        auto w2u = [](const std::wstring& w) -> std::string {
            if (w.empty()) return std::string();
            int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                          nullptr, 0, nullptr, nullptr);
            if (n <= 0) return std::string();
            std::string s(n, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  s.data(), n, nullptr, nullptr);
            return s;
        };
        out_unins_lines += w2u(spec.root);
        out_unins_lines += "|";
        out_unins_lines += w2u(spec.subkey);
        out_unins_lines += "|";
        out_unins_lines += w2u(spec.value_type);
        out_unins_lines += "|";
        out_unins_lines += w2u(spec.value_name);
        out_unins_lines += "|";
        out_unins_lines += w2u(spec.value_data);
        out_unins_lines += "|";
        out_unins_lines += spec.unins_delete_key ? "1" : "0";
        out_unins_lines += "\n";
    }
    return all_ok;
}

void unapply_registry_entries(const ParsedPayload& payload,
                              const std::wstring& install_dir) {
    int count = 0;
    try { count = std::stoi(payload.get("RegCount", "0")); } catch (...) { count = 0; }
    for (int i = 0; i < count; ++i) {
        std::string line = payload.get("Reg" + std::to_string(i), "");
        if (line.empty()) continue;
        RegValueSpec spec;
        if (!parse_reg_entry(line, install_dir, spec)) continue;
        delete_reg_entry(spec);
    }
}

} // namespace stub
