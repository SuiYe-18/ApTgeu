/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - .atui theme file parser
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atu {

struct AtuiTheme {
    // [Window]
    int window_width = 960;
    int window_height = 600;
    bool resizable = false;
    std::wstring title = L"ApTgeu Setup";

    // [Colors] 0xRRGGBB -> 8-bit RGB (stored as 0x00RRGGBB)
    uint32_t primary        = 0x2A9D8F;
    uint32_t primary_hover  = 0x3AB0A3;
    uint32_t background     = 0x1E1E1E;
    uint32_t panel          = 0x252525;
    uint32_t text           = 0xE8E8E8;
    uint32_t text_dim       = 0x9A9A9A;
    uint32_t button         = 0x2A9D8F;
    uint32_t button_text    = 0xFFFFFF;
    uint32_t border         = 0x3A3A3A;

    // [Images] raw PNG bytes (empty = no image)
    std::vector<uint8_t> welcome_banner_png;
    std::vector<uint8_t> sidebar_image_png;
    std::vector<uint8_t> background_image_png;
    std::vector<uint8_t> footer_logo_png;

    // [Layout]
    int sidebar_width   = 180;
    int titlebar_height = 36;
    int button_height   = 32;
    int button_width    = 120;
    int item_spacing_y  = 8;
    int margin          = 16;

    // [Font]
    int font_size = 18;

    // [Pages] v0.5: which wizard pages to show. Default all true.
    struct Pages {
        bool Welcome   = true;
        bool License   = true;
        bool Components = true;
        bool DirSelect = true;
        bool Ready     = true;
        bool Finished  = true;
    } pages;
};

// Parse a .atui file. Missing file -> returns false, theme left as defaults (no exception).
// Missing image paths / unreadable images -> leave corresponding vector empty (no exception).
// base_dir: directory used to resolve relative [Images] paths (typically the .atu file's dir).
bool ParseAtuiFile(const std::string& path, const std::string& base_dir, AtuiTheme& out);

// Internal: wstring title -> UTF-8 bytes (used by payload serializer).
std::string AtuiTitleToUtf8(const AtuiTheme& t);

} // namespace atu
