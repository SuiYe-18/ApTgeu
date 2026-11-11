/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu stub - payload reader / writer / uninstall helpers.
//
// Reads the footer at the end of the running exe (magic "APTGEU01" + uint64
// payload_start_offset), parses the payload blob, and provides helpers to
// lay files down to disk, write unins000.dat, and clean up on uninstall.
//
// Payload binary layout (little-endian, frozen with compiler subagent B):
//   uint32_t config_text_size
//   byte[config_text_size] config_text   (UTF-8, "Key=Value\n" lines)
//   uint32_t file_count
//   repeat file_count:
//       uint32_t rel_path_len
//       byte[rel_path_len] rel_path      (UTF-8, forward slashes)
//       uint32_t raw_size
//       uint32_t crc32                   (IEEE, poly 0xEDB88320)
//       byte[raw_size] file_data
//
// Special rel_paths:
//   "APTGEU::welcome.png"  -> welcome image bytes (kept in memory)
//   "APTGEU::license.txt"  -> license text (kept in memory)
//   anything else          -> installed under installDir

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace stub {

extern std::string g_password;  // v0.2.5: AES-256-CBC password (from /PASSWORD=)
extern bool        g_payload_encrypted;  // v0.2.5: true if footer magic == APTGEU07
extern bool        g_payload_tampered;   // M-02: true if HMAC-SHA256 verify failed

struct PayloadFile {
    std::string rel_path;            // UTF-8, forward slashes
    uint32_t    raw_size = 0;
    uint32_t    crc32    = 0;
    std::string component;           // v0.4: component name (empty = always install)
    std::string tasks;               // v0.14: tasks name (empty = always install)
    std::string flags;               // v0.19: file flags
    std::vector<uint8_t> data;
    // v0.2.2: per-file MD5, present only when payload config has VerifyHash=md5.
    uint8_t     md5[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    bool        has_md5 = false;
};

// v0.14: task entry
struct TaskEntry {
    std::string name;
    std::string description;
    bool checked = true;
};

// Parsed .atui UI theme block embedded in the payload.
// If loaded==false, caller should use built-in dark defaults (800x500, no images).
struct UiTheme {
    bool loaded = false;

    int32_t window_width  = 960;
    int32_t window_height = 600;
    int32_t resizable     = 0;
    std::string title;                       // UTF-8

    // 9 colors, each RGBA bytes (0-255).
    uint8_t primary[4]       = { 42, 157, 143, 255 };
    uint8_t primary_hover[4]  = { 58, 176, 163, 255 };
    uint8_t background[4]    = { 30,  30,  30, 255 };
    uint8_t panel[4]          = { 37,  37,  37, 255 };
    uint8_t text[4]           = { 232, 232, 232, 255 };
    uint8_t text_dim[4]       = { 154, 154, 154, 255 };
    uint8_t button[4]         = { 42, 157, 143, 255 };
    uint8_t button_text[4]    = { 255, 255, 255, 255 };
    uint8_t border[4]         = { 58,  58,  58, 255 };

    // 7 layout/int values.
    int32_t sidebar_width      = 180;
    int32_t titlebar_height   = 36;
    int32_t button_height     = 32;
    int32_t button_width      = 120;
    int32_t item_spacing_y    = 8;
    int32_t margin            = 16;
    int32_t font_size         = 18;

    // 4 images (PNG bytes). Empty vector = no image.
    std::vector<uint8_t> welcome_banner_png;
    std::vector<uint8_t> sidebar_image_png;
    std::vector<uint8_t> background_image_png;
    std::vector<uint8_t> footer_logo_png;
};

struct ParsedPayload {
    std::map<std::string, std::string> config;   // keys: AppName, AppVersion, Publisher,
                                                 //       DefaultDirName, UninstallDisplayName, RegKeyName
    UiTheme theme;                               // embedded .atui block (loaded==false if absent)
    std::vector<PayloadFile> files;              // all entries (special + regular)
    std::string welcome_png;                     // legacy APTGEU::welcome.png (UI block takes priority)
    std::string license_text;                    // UTF-8 text of APTGEU::license.txt (may be empty)
    std::vector<uint8_t> unins_exe_data;         // APTGEU::unins.exe standalone uninstaller PE bytes
    std::vector<uint8_t> unins_ini_data;         // APTGEU::unins000.ini uninstall theme bytes
    std::vector<uint8_t> vcredist_data;          // v0.22: APTGEU::vcredist_x64.exe bootstrapper bytes
    std::vector<TaskEntry> tasks;                // v0.14: [Tasks] entries

    // Convenience accessors.
    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = config.find(key);
        return it == config.end() ? def : it->second;
    }
};

// Read own exe footer + payload. Returns false and fills err on failure
// (e.g. bare stub.exe without payload, IO error, bad magic, bad CRC).
bool read_self_payload(ParsedPayload& out, std::string& err);

// ---- Filesystem helpers (wide-char paths) ----

// Recursively create directory. Returns true if it already existed or was created.
bool ensure_dir_recursive(const std::wstring& path);

// UTF-8 string -> wide string (CP_UTF8).
std::wstring utf8_to_wide(const std::string& s);

// Wide string -> UTF-8.
std::string wide_to_utf8(const std::wstring& s);

// Write every regular (non-special) file from payload into install_dir.
// Fills written_paths with the absolute wide path of each file written.
// Verifies CRC32 after write (re-reads). Returns false on first failure.
bool install_files(const ParsedPayload& p,
                   const std::wstring& install_dir,
                   std::vector<std::wstring>& written_paths,
                   std::string& err,
                   const std::set<std::string>* selected_components = nullptr,
                   const std::set<std::string>* selected_tasks = nullptr);

// Write unins000.dat (UTF-8, one absolute wide path per line) next to install_dir.
bool write_unins_dat(const std::wstring& install_dir,
                     const std::vector<std::wstring>& written_paths);

// Copy current exe (the running stub) to dest_path.
bool copy_self_to(const std::wstring& dest_path);

// Write the standalone uninstaller PE (from APTGEU::unins.exe payload entry)
// to installDir\unins000.exe. NOT added to written_paths (self-deleted).
bool install_unins_exe(const std::vector<uint8_t>& data,
                      const std::wstring& install_dir);

// v0.12: Write uninstall theme ini (APTGEU::unins000.ini) to installDir\unins000.ini.
bool install_unins_ini(const std::vector<uint8_t>& data,
                       const std::wstring& install_dir);

// ---- Uninstall helpers ----

// Read unins000.dat next to own exe and delete every file listed.
// Returns number of files deleted. On failure, returns -1.
int uninstall_delete_listed_files(const std::wstring& dat_path);

// Recursively remove a directory and all its contents (best-effort).
// Skips files that are in use; caller should schedule self-delete separately.
void uninstall_remove_tree(const std::wstring& dir);

// Spawn a detached cmd that waits a moment, then deletes self_path and
// removes install_dir. Used so unins000.exe can delete itself.
void schedule_self_delete(const std::wstring& self_path,
                          const std::wstring& install_dir);

// Create Start Menu / Desktop / Quick Launch .lnk shortcuts for the installed
// app. Returns the list of .lnk paths created (so caller can add them to
// unins000.dat for later cleanup). On failure returns partial list.
// If group_name is non-empty, the Start Menu shortcut is placed in a
// sub-folder Programs\<group_name>\<app>.lnk (Inno /GROUP=...).
//
// v0.24: the three bools gate which shortcuts are created. They default to
// (desktop=true, progmenu=true, quicklaunch=false) which reproduces the legacy
// behavior. Callers wire these to the CreateDesktopIcon / CreateProgramMenuIcon
// / CreateQuickLaunchIcon task check states.
std::vector<std::wstring> create_shortcuts(const std::wstring& install_dir,
                                           const std::wstring& app_name,
                                           const std::wstring& exe_relative_path,
                                           const std::wstring& group_name = L"",
                                           bool create_desktop = true,
                                           bool create_progmenu = true,
                                           bool create_quicklaunch = false);

} // namespace stub
