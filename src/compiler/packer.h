/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu compiler - packer: orchestrates script -> payload -> setup.exe
#pragma once

#include <string>
#include <vector>
#include "atu_lexer.h"
#include "variable.h"
#include "payload_format.h"

namespace atu {

struct PackResult {
    std::string output_path;
    uint64_t output_size = 0;
    uint32_t file_count = 0;       // number of payload entries (including special)
    uint64_t stub_size = 0;
    uint32_t ui_block_size = 0;   // 0 if no theme embedded
    uint64_t payload_raw_size = 0;          // v0.3: uncompressed payload size
    uint64_t payload_compressed_size = 0;  // v0.3: deflated payload size
};

// High-level entry: compile an .atu script into a final setup.exe.
// - atu_path: path to .atu (absolute or relative to cwd)
// - stub_path: path to stub.exe (typically next to aptgeu.exe)
// - ui_override: if non-empty, overrides UiFile from [Setup] (CLI --ui).
// Throws std::runtime_error on any failure.
PackResult pack(const std::string& atu_path,
                const std::string& stub_path,
                const std::string& ui_override = "");

// Resolve where aptgeu.exe itself lives (for finding stub.exe next to it).
std::string own_exe_dir();

// v0.2: --gen-rc mode. Read .atu, copy SetupIcon.ico (if any) to <out-dir>\stub_icon.ico,
// and emit <out-dir>\stub.rc with MAINICON + VERSIONINFO filled from [Setup] fields.
// Returns the list of files produced (stub_icon.ico path, stub.rc path).
// Never throws on a missing SetupIcon (just skips the ICON line).
struct GenRcResult {
    std::string rc_path;
    std::string icon_path;     // empty if no icon was copied
    bool icon_copied = false;
};
GenRcResult gen_rc(const std::string& atu_path, const std::string& out_dir);

} // namespace atu
