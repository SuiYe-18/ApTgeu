/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu stub - HKCU uninstall registry key read/write/delete.
//
// Key: HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\<RegKeyName>
// Values written on install:
//   DisplayName, DisplayVersion, Publisher, InstallLocation,
//   UninstallString, NoModify (DWORD=1), NoRepair (DWORD=1)

#pragma once

#include <string>

namespace stub {

// Write (create or overwrite) the uninstall key. Returns true on success.
// v0.20: display_icon (optional, empty = skip) writes the DisplayIcon value.
bool write_uninstall_registry(const std::wstring& reg_key_name,
                              const std::wstring& display_name,
                              const std::wstring& display_version,
                              const std::wstring& publisher,
                              const std::wstring& install_location,
                              const std::wstring& uninstall_string,
                              const std::wstring& display_icon = L"");

// Delete the whole uninstall key (and all values). Returns true on success.
bool delete_uninstall_registry(const std::wstring& reg_key_name);

// ---------------------------------------------------------------------------
// v0.23: generic [Registry] section support.
//
// One user-declared registry entry. `root` is one of "HKCU"/"HKLM"/"HKCR"
// (case-insensitive). `value_type` is "string" (REG_SZ) or "dword" (REG_DWORD).
// `value_name` empty means the (Default) value. `unins_delete_key` mirrors the
// [Registry] Flags: uninsdeletekey flag - when true, uninstall removes the
// whole key recursively; otherwise only the named value is deleted.
struct RegValueSpec {
    std::wstring root;
    std::wstring subkey;
    std::wstring value_type;
    std::wstring value_name;
    std::wstring value_data;
    bool         unins_delete_key = false;
};

// Parse a wire-format string "root|subkey|valuetype|valuename|valuedata|uninsdeletekey"
// into a RegValueSpec. `install_dir` is used to substitute {app} occurrences.
// Returns false if the root/subkey are missing or root is unknown.
bool parse_reg_entry(const std::string& line_utf8,
                     const std::wstring& install_dir,
                     RegValueSpec& out);

// Create/open the key and write the value. Returns true on success.
bool write_reg_entry(const RegValueSpec& spec);

// Delete a previously-written registry entry. If unins_delete_key is true,
// recursively deletes the whole key; otherwise deletes only the named value.
// Best-effort: returns true if the resulting state is "gone".
bool delete_reg_entry(const RegValueSpec& spec);

// Convenience: read RegCount/Reg0..RegN out of a config map (UTF-8 -> UTF-8
// strings), substitute {app} with install_dir, write each entry to the real
// registry, and append the wire-format lines (already {app}-substituted in
// UTF-8) to `out_unins_lines` so the caller can persist them into
// unins000_extra.ini for the standalone uninstaller. Returns false if any
// entry failed to write.
struct ParsedPayload;  // fwd
bool apply_registry_entries(const ParsedPayload& payload,
                            const std::wstring& install_dir,
                            std::string& out_unins_lines);

// Reverse of apply_registry_entries: parse RegCount/RegN from the payload and
// delete each entry (used when the stub itself is invoked as the uninstaller,
// i.e. /UNINSTALL without an embedded standalone unins.exe).
void unapply_registry_entries(const ParsedPayload& payload,
                              const std::wstring& install_dir);

} // namespace stub
