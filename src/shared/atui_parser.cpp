/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "atui_parser.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>

namespace atu {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) --e;
    return s.substr(b, e - b);
}

// UTF-8 -> wstring (MVP: simple codepoint-by-codepoint, no surrogate pairing needed for BMP ASCII).
std::wstring utf8_to_ws(const std::string& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (c < 0x80) { out.push_back((wchar_t)c); i++; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F);
            out.push_back((wchar_t)cp); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F);
            out.push_back((wchar_t)cp); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) |
                          ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F);
            // Encode as UTF-16 surrogate pair
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
            i += 4;
        } else {
            out.push_back((wchar_t)c); i++;
        }
    }
    return out;
}

std::string ws_to_utf8(const std::wstring& w) {
    std::string out;
    size_t i = 0;
    while (i < w.size()) {
        uint32_t cp = (uint32_t)w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t lo = (uint32_t)w[i+1];
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else { i++; }
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Parse "0xRRGGBB" -> 0x00RRGGBB. Returns true on success.
bool parse_color(const std::string& s, uint32_t& out) {
    std::string t = trim(s);
    if (t.size() >= 2 && (t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))) t = t.substr(2);
    if (t.size() != 6) return false;
    uint32_t v = 0;
    for (char c : t) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

bool parse_bool(const std::string& s, bool def) {
    std::string t = trim(s);
    std::string low; low.reserve(t.size());
    for (char c : t) low.push_back((char)std::tolower((unsigned char)c));
    if (low == "1" || low == "yes" || low == "true" || low == "on") return true;
    if (low == "0" || low == "no" || low == "false" || low == "off") return false;
    return def;
}

int parse_int(const std::string& s, int def) {
    std::string t = trim(s);
    if (t.empty()) return def;
    try { return std::stoi(t); } catch (...) { return def; }
}

bool file_exists(const std::string& p) {
    struct _stat st;
    return ::_stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFMT) != _S_IFDIR;
}

std::vector<uint8_t> read_file_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

// Resolve possibly-relative path against base_dir. If already absolute, return as-is.
std::string resolve_path(const std::string& p, const std::string& base_dir) {
    if (p.empty()) return p;
    bool abs = false;
    if (p.size() >= 2 && p[1] == ':') abs = true;
    if (p.size() >= 1 && (p[0] == '/' || p[0] == '\\')) abs = true;
    if (abs) return p;
    return base_dir + "\\" + p;
}

} // namespace

bool ParseAtuiFile(const std::string& path, const std::string& base_dir, AtuiTheme& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    std::istringstream in(text);
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));

        if (section == "Window") {
            if      (k == "Title")    out.title = utf8_to_ws(v);
            else if (k == "Width")    out.window_width  = parse_int(v, out.window_width);
            else if (k == "Height")   out.window_height = parse_int(v, out.window_height);
            else if (k == "Resizable") out.resizable = parse_bool(v, out.resizable);
        } else if (section == "Colors") {
            uint32_t c;
            if (!parse_color(v, c)) continue;
            if      (k == "Primary")       out.primary       = c;
            else if (k == "PrimaryHover")  out.primary_hover = c;
            else if (k == "Background")    out.background    = c;
            else if (k == "Panel")         out.panel         = c;
            else if (k == "Text")          out.text          = c;
            else if (k == "TextDim")       out.text_dim      = c;
            else if (k == "Button")        out.button        = c;
            else if (k == "ButtonText")    out.button_text   = c;
            else if (k == "Border")        out.border        = c;
        } else if (section == "Images") {
            if (v.empty()) continue;
            std::string full = resolve_path(v, base_dir);
            if (!file_exists(full)) continue; // silently skip
            auto bytes = read_file_binary(full);
            if      (k == "WelcomeBanner")  out.welcome_banner_png  = std::move(bytes);
            else if (k == "SidebarImage")   out.sidebar_image_png   = std::move(bytes);
            else if (k == "BackgroundImage") out.background_image_png = std::move(bytes);
            else if (k == "FooterLogo")     out.footer_logo_png     = std::move(bytes);
        } else if (section == "Layout") {
            if      (k == "SidebarWidth")    out.sidebar_width   = parse_int(v, out.sidebar_width);
            else if (k == "TitleBarHeight")  out.titlebar_height = parse_int(v, out.titlebar_height);
            else if (k == "ButtonHeight")   out.button_height   = parse_int(v, out.button_height);
            else if (k == "ButtonWidth")    out.button_width     = parse_int(v, out.button_width);
            else if (k == "ItemSpacingY")    out.item_spacing_y   = parse_int(v, out.item_spacing_y);
            else if (k == "Margin")         out.margin           = parse_int(v, out.margin);
        } else if (section == "Font") {
            if (k == "Size") out.font_size = parse_int(v, out.font_size);
        } else if (section == "Pages") {
            auto truthy = [](const std::string& s) {
                return s == "yes" || s == "true" || s == "1" || s == "on";
            };
            if      (k == "Welcome")    out.pages.Welcome    = truthy(v);
            else if (k == "License")    out.pages.License    = truthy(v);
            else if (k == "Components") out.pages.Components = truthy(v);
            else if (k == "DirSelect")  out.pages.DirSelect  = truthy(v);
            else if (k == "Ready")      out.pages.Ready      = truthy(v);
            else if (k == "Finished")   out.pages.Finished   = truthy(v);
        }
    }
    // expose ws_to_utf8 via a tiny static helper for serialization use
    // (kept here to avoid leaking wstring->utf8 into header)
    return true;
}

// Expose for payload_format.cpp
std::string AtuiTitleToUtf8(const AtuiTheme& t) {
    std::wstring w = t.title;
    std::string out;
    size_t i = 0;
    while (i < w.size()) {
        uint32_t cp = (uint32_t)w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t lo = (uint32_t)w[i+1];
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else { i++; }
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace atu
