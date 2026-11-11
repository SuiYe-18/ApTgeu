/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu stub - [INI] section support.
//
// One user-declared [INI] entry. The stub calls WritePrivateProfileStringW at
// install time to create/overwrite a key=value pair inside a section of an INI
// file. At uninstall time, if `unins_delete_entry` is true, the stub calls
// WritePrivateProfileStringW with a NULL value to delete the key.
//
// Wire format (payload config):
//   IniN = filename|section|key|value|flags
//   IniCount = N
// where `flags` is a space-separated string that may contain:
//   createifdoesntexist  - create the INI file if it does not already exist
//                          (default: only write when the file exists).
//   uninsdeleteentry     - on uninstall, delete this key from the INI file.
//
// {app} in `filename` is substituted by the stub at runtime.

#pragma once

#include <string>

namespace stub {

struct ParsedPayload;  // fwd

struct IniSpec {
    std::wstring filename;          // absolute path after {app} substitution
    std::wstring section;
    std::wstring key;
    std::wstring value;
    bool         unins_delete_entry   = false;
    bool         create_if_doesnt_exist = false;
};

// Parse a wire-format string "filename|section|key|value|flags" into an
// IniSpec. `install_dir` is used to substitute {app} in `filename`.
// Returns false if filename/section/key are missing.
bool parse_ini_entry(const std::string& line_utf8,
                     const std::wstring& install_dir,
                     IniSpec& out);

// Call WritePrivateProfileStringW to write section/key=value into filename.
// If `create_if_doesnt_exist` is false and the file does not exist, the entry
// is skipped (logged) and returns true (nothing to do).
bool write_ini_entry(const IniSpec& spec);

// Call WritePrivateProfileStringW with a NULL value to delete the key.
// Best-effort: returns true if the resulting state is "gone".
bool delete_ini_entry(const IniSpec& spec);

// Convenience: read IniCount/IniN out of the payload config, substitute {app}
// in filename, write each entry, and append the wire-format lines (already
// {app}-substituted in UTF-8) to `out_unins_lines` so the standalone
// uninstaller can persist them into unins000_extra.ini.
bool apply_ini_entries(const ParsedPayload& payload,
                       const std::wstring& install_dir,
                       std::string& out_unins_lines);

// Reverse of apply_ini_entries: parse IniCount/IniN from the payload and
// delete each entry that has uninsdeleteentry set. Used when the stub itself
// is invoked as the uninstaller.
void unapply_ini_entries(const ParsedPayload& payload,
                         const std::wstring& install_dir);

} // namespace stub
