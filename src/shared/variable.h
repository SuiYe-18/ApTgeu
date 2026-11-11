/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - variable substitution ({app}, {autopf}, {#AppVersion}, ...)
#pragma once

#include <string>
#include <map>
#include <cstdint>

namespace atu {

struct AtuScript;

// After parsing the raw script, compute substituted values.
// Order:
//   pass 1: {autopf} -> C:\Program Files ; {autopf32} -> C:\Program Files (x86)
//           {#AppVersion} -> AppVersion ; {#AppName} -> AppName
//   DefaultDirName_final = pass1(DefaultDirName)
//   pass 2: {app} -> DefaultDirName_final
struct Resolved {
    std::string app_name;
    std::string app_version;
    std::string publisher;
    std::string default_dir;          // fully substituted install dir
    std::string uninstall_display_name; // fully substituted
    std::string reg_key_name;
    std::string welcome_image;         // raw source path (empty if not set)
    std::string license_file;          // raw source path (empty if not set)
    std::string output_dir;
    std::string output_base_name;
    std::string language;              // v0.6: "zh" or "en"
    std::string compression;           // v0.6: "none"|"zlib"|"lzma2"|"se"|"zstd" (default lzma2)
    int         lzma_level = 9;        // perf: CompressionLevel 1..9 (default 9)
    uint32_t    lzma_dict  = 1u<<27;   // perf: CompressionDictionary bytes (default 128MB)
    std::string uninstall_ui_file;     // v0.12: path to .atu0 uninstall theme
    // v0.13 B4
    std::string app_publisher_url;
    std::string app_support_url;
    std::string updates_url;
    std::string readme_file;
    std::string min_version;
    std::string default_group_name;

    // v0.20: [Setup] wizard / uninstall flags.
    bool        disable_welcome_page    = false;
    bool        disable_readme_page     = false;
    bool        disable_dir_page        = false;
    bool        disable_components_page = false;
    bool        disable_ready_page      = false;
    bool        disable_finished_page   = false;
    std::string wizard_style;            // "modern" | "classic" (informational)
    bool        wizard_resizable        = false;
    std::string uninstall_display_icon;  // path to .ico/.exe for registry DisplayIcon
    bool        allow_no_icons          = false;
    bool        allow_root_directory    = false;
    std::string code_funcs;             // v0.21: comma-separated Pascal procedure/function names from [Code]

    // v0.22: VC++ 2015-2022 x64 redistributable auto-detect / install.
    // Enabled by [Setup] InstallVcRedist=yes. The compiler embeds the
    // bootstrapper as a special payload entry "APTGEU::vcredist_x64.exe";
    // the stub queries the registry and only runs it when the runtime is
    // missing.
    bool        install_vcredist = false;
    std::string vcredist_source;        // raw path from VcRedistSource=... (may be empty)
    std::string vcredist_url;           // v0.23: download URL from VcRedistURL=... (may be empty)
    std::string vcredist_force;         // v0.23: test hook VcRedistForceInstall=1 (skip detection)
};

Resolved resolve_script(const AtuScript& scr);

// Apply substitution passes to an arbitrary raw string, given the resolved context.
// (Exposed so packer can substitute Source/DestDir lines too.)
std::string substitute(const std::string& raw, const Resolved& r);

} // namespace atu
