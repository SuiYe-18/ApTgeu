/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - IEEE CRC32 (table-driven, poly 0xEDB88320, matches zlib crc32)
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace atu {

// Compute CRC32 over data. Standard zlib CRC32 polynomial 0xEDB88320.
uint32_t crc32(const void* data, size_t len);
uint32_t crc32(const std::vector<uint8_t>& data);
uint32_t crc32(const std::string& data);

} // namespace atu
