/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - .atu script parser (ini-style, minimal)
#pragma once

#include <string>
#include <map>
#include <vector>

namespace atu {

// One [Files] entry, after splitting on ; and then :
struct FileEntry {
    std::string source;    // Source: "..." raw path (not yet variable-substituted)
    std::string destdir;   // DestDir: "..." raw path
    std::string flags;     // Flags: "..." (optional, may be empty)
    std::string component; // v0.4: Components: "..." (optional, empty = main)
    std::string tasks;     // v0.14: Tasks: "..." (optional, comma-separated)
};

// One [Components] entry
struct ComponentEntry {
    std::string name;
    std::string description;
};

// v0.14: [Run] / [UninstallRun] entry
struct RunEntry {
    std::string filename;    // Filename: "{app}\xxx.exe"
    std::string parameters;  // Parameters: "..." (optional)
    std::string workingdir;  // WorkingDir: "..." (optional)
    std::string description; // Description: "Launch app"
    std::string flags;       // nowait skipifsilent postinstall runhidden skipifsourcedoesntexist
};

// v0.14: [Tasks] entry
struct TaskEntry {
    std::string name;
    std::string description;
    bool unchecked = false;
};

// v0.14: [Registry] entry
struct RegEntry {
    std::string root;       // HKCU HKLM HKCR HKA
    std::string subkey;
    std::string valuetype;  // string dword
    std::string valuename;
    std::string valuedata;
    bool uninsdeletekey = false;
};

// v0.15: [INI] entry
struct IniEntry {
    std::string filename;   // Filename: "{app}\config.ini"
    std::string section;    // Section: "Settings"
    std::string key;        // Key: "Version"
    std::string value;      // String: "1.0"
    std::string flags;      // v0.24: raw Flags string (createifdoesntexist, uninsdeleteentry)
};

// v0.15: [InstallDelete]/[UninstallDelete] entry
struct DelEntry {
    std::string type;       // files filesandordirs dirifempty
    std::string name;       // Name: "{app}\cache"
};

// v0.16: [Icons] entry
struct IconEntry {
    std::string name;
    std::string filename;
    std::string icon_filename;
    std::string comment;
    std::string parameters;
    std::string workingdir;
};

// v0.17: [Types] entry (setup types)
struct TypeEntry {
    std::string name;
    std::string description;
};

// v0.17: [Dirs] entry (explicit empty dirs)
struct DirEntry {
    std::string name;
    std::string flags;
};

// v0.23: [InstallService] entry - Windows service installed via SCM.
struct ServiceEntry {
    std::string name;          // Name: service key name (e.g. "MySvc")
    std::string display_name;  // DisplayName: friendly name shown in services.msc
    std::string description;   // Description: optional tooltip text
    std::string start_type;    // StartType: auto | demand | disabled
    std::string executable;    // Executable: "{app}\\mysvc.exe" (ImagePath)
};

struct AtuScript {
    // [Setup] section: key -> raw value (unsubstituted)
    std::map<std::string, std::string> setup;
    // [Files] section entries
    std::vector<FileEntry> files;
    // v0.4: [Components] section entries (order preserved)
    std::vector<ComponentEntry> components;
    // v0.14
    std::vector<RunEntry> run_entries;
    std::vector<TaskEntry> tasks;
    std::vector<RegEntry> reg_entries;
    std::vector<IniEntry> ini_entries;      // v0.15
    std::vector<DelEntry> install_delete;   // v0.15
    std::vector<DelEntry> unins_delete;     // v0.15
    std::vector<IconEntry> icons;           // v0.16
    std::vector<TypeEntry> types;           // v0.17
    std::vector<DirEntry> dirs;             // v0.17
    std::vector<RunEntry> install_run;      // v0.17 [InstallRun]
    std::vector<RunEntry> unins_run;        // v0.24 [UninstallRun] (was lumped into run_entries)
    std::vector<ServiceEntry> services;     // v0.23 [InstallService]

    // Helpers
    bool has(const std::string& key) const { return setup.count(key) > 0; }
    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = setup.find(key);
        return it == setup.end() ? def : it->second;
    }
};

// Parse a UTF-8 (or ASCII) .atu file. Throws std::runtime_error on malformed input.
AtuScript parse_atu_file(const std::string& path);

// Parse from in-memory text. Exposed for testing.
AtuScript parse_atu_text(const std::string& text);

} // namespace atu
