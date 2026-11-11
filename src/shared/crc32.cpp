/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "crc32.h"

namespace atu {

namespace {

uint32_t g_table[256];
bool g_table_ready = false;

void build_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_table[i] = c;
    }
    g_table_ready = true;
}

} // namespace

uint32_t crc32(const void* data, size_t len) {
    if (!g_table_ready) build_table();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    return crc32(data.data(), data.size());
}

uint32_t crc32(const std::string& data) {
    return crc32(data.data(), data.size());
}

} // namespace atu
