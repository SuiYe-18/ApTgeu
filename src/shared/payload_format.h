/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - payload binary format (must match stub reader byte-for-byte)
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "variable.h"
#include "atui_parser.h"
#include "atu_lexer.h"

namespace atu {

// One file entry inside payload.
struct PayloadFile {
    std::string rel_path;            // UTF-8, forward slashes. Special: "APTGEU::welcome.png", "APTGEU::license.txt"
    std::string component;            // v0.4: component name (empty = unassigned / default)
    std::string tasks;                // v0.14: tasks name (empty = always install)
    std::string flags;                // v0.19: file flags string
    std::vector<uint8_t> data;
    uint8_t compression = 0;          // v0.11: 0=store, 1=lzma2 (per-file)
    // v0.2.2: MD5 of `data`, only written to the wire when verify_md5=true
    // (controlled by [Setup] VerifyHash=md5).
    uint8_t md5[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
};

// Build the full uncompressed payload bytes (compression happens in packer.cpp).
// Layout:
//   uint32 config_text_size
//   byte[config_text_size] config_text (UTF-8, each line "Key=Value\n")
//   uint32 ui_block_size        (v0.1.1: 0 if theme == nullptr)
//   byte[ui_block_size] ui_block
//   uint32 file_count
//   repeated:
//     uint32 rel_path_len
//     byte[rel_path_len] rel_path
//     uint32 component_len      (v0.4)
//     byte[component_len] component_name
//     uint32 tasks_len          (v0.14)
//     byte[tasks_len] tasks
//     uint32 flags_len          (v0.19)
//     byte[flags_len] flags
//     uint32 raw_size
//     uint32 crc32
//     byte   compression
//     byte[16] md5              (v0.2.2: only when verify_md5 == true;
//                                enabled via [Setup] VerifyHash=md5)
//     byte[raw_size] data
std::vector<uint8_t> build_payload(const Resolved& r,
                                   const std::vector<PayloadFile>& files,
                                   const AtuiTheme* theme = nullptr,
                                   const std::vector<ComponentEntry>* components = nullptr,
                                   const std::vector<TaskEntry>* tasks = nullptr,
                                   bool verify_md5 = false,
                                   const std::vector<RegEntry>* reg_entries = nullptr,
                                   const std::vector<ServiceEntry>* services = nullptr,
                                   const std::vector<IniEntry>* ini_entries = nullptr,
                                   const std::vector<RunEntry>* unins_run = nullptr,
                                   const std::vector<RunEntry>* install_run = nullptr);

// Serialize the UI theme block (fixed binary layout, all little-endian).
// Empty images -> length 0. Called by build_payload; exposed for stub-side symmetry.
void SerializeUiBlock(const AtuiTheme& theme, std::vector<uint8_t>& out);

// Build the 16-byte footer: magic (8 bytes) + uint64 LE payload_start_offset.
// Default magic = "APTGEU03" (LZMA2). Pass explicit magic for other codecs.
std::vector<uint8_t> build_footer(uint64_t payload_start_offset, const char magic[8]);
std::vector<uint8_t> build_footer(uint64_t payload_start_offset);
std::vector<uint8_t> build_footer_v16(uint64_t payload_start_offset,
                                       uint64_t total_payload_size,
                                       uint32_t current_slice);

// Helpers (exposed for testing / stub side)
void write_u32_le(std::vector<uint8_t>& out, uint32_t v);
void write_u64_le(std::vector<uint8_t>& out, uint64_t v);
void write_i32_le(std::vector<uint8_t>& out, int32_t v);
void write_bytes(std::vector<uint8_t>& out, const void* p, size_t n);
void write_bytes(std::vector<uint8_t>& out, const std::string& s);
void write_bytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& v);

} // namespace atu
