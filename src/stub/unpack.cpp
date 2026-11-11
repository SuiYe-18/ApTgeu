/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "unpack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <zlib.h>
#include <Lzma2Dec.h>
#include <zstd.h>
#include "se.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "crc32.h"   // from src/shared
#include "md5.h"     // v0.2.2: per-file MD5 integrity check
#include "crypto.h"  // v0.2.5: AES-256-CBC payload decryption
#include "log.h"     // M-01: legacy-KDF / MAC-mismatch warnings

namespace stub {

// Simple malloc/free allocator for LZMA2 decoder (must not be nullptr).
static void* lzma_alloc(ISzAllocPtr, size_t sz) { return std::malloc(sz); }
static void  lzma_free(ISzAllocPtr, void* p) { std::free(p); }
static ISzAlloc g_lzma_alloc = { &lzma_alloc, &lzma_free };

// v0.2.5: password for AES-256-CBC-decrypting payload (set from main.cpp /PASSWORD=
// before read_self_payload). Empty = no decryption needed (unencrypted installer).
std::string g_password;

// v0.2.5: set by read_self_payload when footer magic == "APTGEU07". Caller
// (main.cpp) inspects this AFTER read_self_payload returns false to decide
// whether to pop the "密码错误" MessageBox.
bool g_payload_encrypted = false;

// M-02: set by read_self_payload when the outer footer carries an HMAC-SHA256
// (magic "APTGEU09") and the MAC over the encrypted envelope does NOT match.
// Caller (main.cpp) pops "数据被篡改" instead of "密码错误".
bool g_payload_tampered = false;

// ---------------------------------------------------------------------------
// UTF-8 / wide conversion
// ---------------------------------------------------------------------------

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Little-endian readers
// ---------------------------------------------------------------------------

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

// ---------------------------------------------------------------------------
// Self path
// ---------------------------------------------------------------------------

static std::wstring get_own_exe_path() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0) return L"";
    return std::wstring(buf, n);
}

// ---------------------------------------------------------------------------
// Payload parsing
// ---------------------------------------------------------------------------

static int32_t rd_i32(const uint8_t* p) {
    return (int32_t)rd_u32(p);
}

static void rd_color(const uint8_t* p, uint8_t out[4]) {
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
}

// Parse the ui_block blob. Returns false on malformed input (caller should
// fall back to built-in default theme rather than abort install).
static bool parse_ui_block(const uint8_t* p, size_t size, UiTheme& t) {
    size_t pos = 0;
    auto need = [&](size_t n) -> bool {
        if (pos + n > size) return false;
        return true;
    };
    auto rd_bytes = [&](uint8_t* out, size_t n) -> bool {
        if (!need(n)) return false;
        std::memcpy(out, p + pos, n); pos += n;
        return true;
    };
    auto rd_i32v = [&](int32_t& v) -> bool {
        if (!need(4)) return false;
        v = rd_i32(p + pos); pos += 4;
        return true;
    };
    auto rd_color = [&](uint8_t out[4]) -> bool {
        if (!need(4)) return false;
        out[0] = p[pos+0]; out[1] = p[pos+1];
        out[2] = p[pos+2]; out[3] = p[pos+3];
        pos += 4;
        return true;
    };

    if (!rd_i32v(t.window_width))      return false;
    if (!rd_i32v(t.window_height))     return false;
    if (!rd_i32v(t.resizable))         return false;

    if (!need(4)) return false;
    uint32_t title_len = rd_u32(p + pos); pos += 4;
    if (!need(title_len)) return false;
    t.title.assign((const char*)(p + pos), title_len); pos += title_len;

    // 9 colors.
    if (!rd_color(t.primary))        return false;
    if (!rd_color(t.primary_hover))  return false;
    if (!rd_color(t.background))     return false;
    if (!rd_color(t.panel))           return false;
    if (!rd_color(t.text))            return false;
    if (!rd_color(t.text_dim))        return false;
    if (!rd_color(t.button))          return false;
    if (!rd_color(t.button_text))     return false;
    if (!rd_color(t.border))          return false;

    // 7 layout/int values.
    if (!rd_i32v(t.sidebar_width))    return false;
    if (!rd_i32v(t.titlebar_height))  return false;
    if (!rd_i32v(t.button_height))    return false;
    if (!rd_i32v(t.button_width))     return false;
    if (!rd_i32v(t.item_spacing_y))   return false;
    if (!rd_i32v(t.margin))           return false;
    if (!rd_i32v(t.font_size))        return false;

    // 4 images: uint32 len + bytes.
    auto rd_image = [&](std::vector<uint8_t>& out) -> bool {
        if (!need(4)) return false;
        uint32_t n = rd_u32(p + pos); pos += 4;
        if (!need(n)) return false;
        out.assign(p + pos, p + pos + n); pos += n;
        return true;
    };
    if (!rd_image(t.welcome_banner_png))   return false;
    if (!rd_image(t.sidebar_image_png))    return false;
    if (!rd_image(t.background_image_png)) return false;
    if (!rd_image(t.footer_logo_png))      return false;

    t.loaded = true;
    return true;
}

static bool parse_payload(const uint8_t* data, size_t size, ParsedPayload& out, std::string& err) {
    size_t pos = 0;
    auto need = [&](size_t n) -> bool {
        if (pos + n > size) { err = "payload truncated"; return false; }
        return true;
    };

    // config_text
    if (!need(4)) return false;
    uint32_t cfg_size = rd_u32(data + pos); pos += 4;
    if (!need(cfg_size)) return false;
    std::string cfg((const char*)(data + pos), cfg_size); pos += cfg_size;

    // Parse "Key=Value\n" lines.
    {
        std::istringstream ss(cfg);
        std::string line;
        while (std::getline(ss, line)) {
            // strip trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            out.config[k] = v;
        }
    }

    // v0.2.2: if compiler wrote VerifyHash=md5, each file carries 16 extra
    // bytes of MD5 after the compression flag.
    bool expect_md5 = (out.get("VerifyHash", "") == "md5");

    // v0.14: parse Tasks from config
    {
        std::string names = out.get("Tasks", "");
        std::string descs = out.get("TasksDesc", "");
        std::string unchecked = out.get("TasksUnchecked", "");
        if (!names.empty()) {
            // split names by comma
            std::vector<std::string> name_list;
            size_t start = 0;
            for (size_t i = 0; i <= names.size(); ++i) {
                if (i == names.size() || names[i] == ',') {
                    name_list.push_back(names.substr(start, i - start));
                    start = i + 1;
                }
            }
            // split descs by |, each "name:desc"
            std::vector<std::string> desc_list;
            start = 0;
            for (size_t i = 0; i <= descs.size(); ++i) {
                if (i == descs.size() || descs[i] == '|') {
                    desc_list.push_back(descs.substr(start, i - start));
                    start = i + 1;
                }
            }
            // unchecked flags by comma
            std::vector<bool> unchk(name_list.size(), false);
            {
                size_t s = 0;
                for (size_t i = 0; i <= unchecked.size(); ++i) {
                    if (i == unchecked.size() || unchecked[i] == ',') {
                        std::string f = unchecked.substr(s, i - s);
                        if (!f.empty() && f[0] == '1') {
                            size_t idx = s == 0 ? 0 : 0; // approximate
                        }
                        s = i + 1;
                    }
                }
                // simpler: just index
                size_t idx = 0;
                std::string cur;
                for (char c : unchecked) {
                    if (c == ',') {
                        if (idx < unchk.size()) unchk[idx] = (cur == "1");
                        idx++; cur.clear();
                    } else cur.push_back(c);
                }
                if (idx < unchk.size()) unchk[idx] = (cur == "1");
            }
            for (size_t i = 0; i < name_list.size() && i < name_list.size(); ++i) {
                TaskEntry t;
                t.name = name_list[i];
                t.checked = !unchk[i];
                if (i < desc_list.size()) {
                    auto colon = desc_list[i].find(':');
                    if (colon != std::string::npos)
                        t.description = desc_list[i].substr(colon + 1);
                }
                out.tasks.push_back(t);
            }
        }
    }

    // ui_block (new in v0.1 theme support). If ui_block_size==0, skip and
    // leave out.theme with built-in defaults.
    if (!need(4)) return false;
    uint32_t ui_block_size = rd_u32(data + pos); pos += 4;
    if (ui_block_size > 0) {
        if (!need(ui_block_size)) return false;
        if (!parse_ui_block(data + pos, ui_block_size, out.theme)) {
            // Malformed theme: keep defaults, do NOT abort install.
            out.theme = UiTheme{};
        }
        pos += ui_block_size;
    }

    // file_count
    if (!need(4)) return false;
    uint32_t file_count = rd_u32(data + pos); pos += 4;

    for (uint32_t i = 0; i < file_count; ++i) {
        if (!need(4)) return false;
        uint32_t rel_len = rd_u32(data + pos); pos += 4;
        if (!need(rel_len)) return false;
        std::string rel((const char*)(data + pos), rel_len); pos += rel_len;

        // v0.4: component_len (0 = uncategorized, always installed).
        if (!need(4)) return false;
        uint32_t comp_len = rd_u32(data + pos); pos += 4;
        if (!need(comp_len)) return false;
        std::string comp((const char*)(data + pos), comp_len); pos += comp_len;

        // v0.14: tasks_len
        if (!need(4)) return false;
        uint32_t tasks_len = rd_u32(data + pos); pos += 4;
        if (!need(tasks_len)) return false;
        std::string tasks((const char*)(data + pos), tasks_len); pos += tasks_len;

        // v0.19: flags_len
        if (!need(4)) return false;
        uint32_t flags_len = rd_u32(data + pos); pos += 4;
        if (!need(flags_len)) return false;
        std::string flags((const char*)(data + pos), flags_len); pos += flags_len;

        if (!need(9)) return false;
        uint32_t raw_size = rd_u32(data + pos); pos += 4;
        uint32_t crc     = rd_u32(data + pos); pos += 4;
        uint8_t comp_flag = data[pos]; pos += 1;  // v0.11: metadata, not used for decompression
        // v0.2.2: optional 16-byte MD5 digest of the raw file data.
        uint8_t md5_bytes[16] = {0};
        if (expect_md5) {
            if (!need(16)) return false;
            std::memcpy(md5_bytes, data + pos, 16);
            pos += 16;
        }
        if (!need(raw_size)) return false;

        PayloadFile pf;
        pf.rel_path = rel;
        pf.component = comp;
        pf.tasks = tasks;
        pf.flags = flags;
        pf.raw_size = raw_size;
        pf.crc32    = crc;
        if (expect_md5) {
            std::memcpy(pf.md5, md5_bytes, 16);
            pf.has_md5 = true;
        }
        pf.data.assign(data + pos, data + pos + raw_size);
        pos += raw_size;

        // CRC check.
        uint32_t actual = atu::crc32(pf.data.data(), pf.data.size());
        if (actual != crc) {
            char b[128];
            std::snprintf(b, sizeof(b), "CRC mismatch on '%s': stored=0x%08x actual=0x%08x",
                          rel.c_str(), crc, actual);
            err = b;
            return false;
        }

        if (rel == "APTGEU::welcome.png") {
            out.welcome_png.assign((const char*)pf.data.data(), pf.data.size());
        } else if (rel == "APTGEU::license.txt") {
            out.license_text.assign((const char*)pf.data.data(), pf.data.size());
        } else if (rel == "APTGEU::unins.exe") {
            out.unins_exe_data.assign(pf.data.begin(), pf.data.end());
        } else if (rel == "APTGEU::unins000.ini") {
            out.unins_ini_data.assign(pf.data.begin(), pf.data.end());
        } else if (rel == "APTGEU::vcredist_x64.exe") {
            // v0.22: VC++ 2015-2022 x64 bootstrapper, kept in memory.
            out.vcredist_data.assign(pf.data.begin(), pf.data.end());
        } else {
            out.files.push_back(std::move(pf));
        }
    }

    return true;
}

bool read_self_payload(ParsedPayload& out, std::string& err) {
    std::wstring self = get_own_exe_path();
    if (self.empty()) { err = "GetModuleFileName failed"; return false; }

    HANDLE h = ::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "CreateFile(self) failed"; return false; }

    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(h, &sz)) { err = "GetFileSizeEx failed"; ::CloseHandle(h); return false; }
    if (sz.QuadPart < 16) { err = "file too small for footer"; ::CloseHandle(h); return false; }

    // Read footer. M-02 v48=48 bytes (AES-CBC+HMAC), v0.16=28 bytes, older=16 bytes.
    uint8_t footer[48] = {0};
    bool v16 = false;
    bool v48 = false;
    uint64_t total_payload_size = 0;
    uint8_t stored_mac[32] = {0};
    {
        // M-02: try 48-byte footer first (magic "APTGEU09" = AES-CBC + HMAC-SHA256).
        LARGE_INTEGER off48; off48.QuadPart = sz.QuadPart - 48;
        if (off48.QuadPart >= 0) {
            ::SetFilePointerEx(h, off48, nullptr, FILE_BEGIN);
            DWORD got48 = 0;
            uint8_t f48[48] = {0};
            if (::ReadFile(h, f48, 48, &got48, nullptr) && got48 == 48 &&
                std::memcmp(f48, "APTGEU09", 8) == 0) {
                v48 = true;
                std::memcpy(footer, f48, 16);            // magic[8] + payload_offset[u64]
                std::memcpy(stored_mac, f48 + 16, 32);   // HMAC-SHA256
            }
        }
        if (!v48) {
            // try 28-byte footer first (v0.16 slicing, magic "APTGEU06")
            LARGE_INTEGER off28; off28.QuadPart = sz.QuadPart - 28;
            if (off28.QuadPart >= 0) {
                ::SetFilePointerEx(h, off28, nullptr, FILE_BEGIN);
                DWORD got28 = 0;
                uint8_t f28[28] = {0};
                if (::ReadFile(h, f28, 28, &got28, nullptr) && got28 == 28 &&
                    std::memcmp(f28, "APTGEU06", 8) == 0) {
                    v16 = true;
                    memcpy(footer, f28, 28);
                    total_payload_size = rd_u64(footer + 16);
                }
            }
        }
        if (!v48 && !v16) {
            LARGE_INTEGER off16; off16.QuadPart = sz.QuadPart - 16;
            ::SetFilePointerEx(h, off16, nullptr, FILE_BEGIN);
            DWORD got16 = 0;
            if (!::ReadFile(h, footer, 16, &got16, nullptr) || got16 != 16) {
                err = "read footer failed"; ::CloseHandle(h); return false;
            }
        }
    }

    // Magic: APTGEU01 (none), 02 (zlib), 03 (LZMA2), 04 (SE), 05 (perFile), 06 (v16 slicing),
    //        07 (v0.2.5 AES-256-CBC encrypted; inner codec magic is the first 8 bytes of payload),
    //        08 (v0.23 zstd), 09 (M-02 AES-256-CBC + HMAC-SHA256, 48-byte footer).
    enum class Comp { None, Zlib, Lzma2, SE, PerFile, Encrypted, Zstd } comp = Comp::None;
    g_payload_encrypted = false;
    g_payload_tampered = false;
    if (v48) {
        g_payload_encrypted = true;
        comp = Comp::Encrypted;
    } else if (std::memcmp(footer, "APTGEU07", 8) == 0) {
        g_payload_encrypted = true;
        comp = Comp::Encrypted;
    } else if (std::memcmp(footer, "APTGEU08", 8) == 0) {
        comp = Comp::Zstd;
    } else if (std::memcmp(footer, "APTGEU05", 8) == 0) {
        comp = Comp::PerFile;
    } else if (std::memcmp(footer, "APTGEU04", 8) == 0) {
        comp = Comp::SE;
    } else if (std::memcmp(footer, "APTGEU03", 8) == 0) {
        comp = Comp::Lzma2;
    } else if (std::memcmp(footer, "APTGEU02", 8) == 0) {
        comp = Comp::Zlib;
    } else if (std::memcmp(footer, "APTGEU06", 8) == 0) {
        // v0.16: magic is same codec as before, stored in... actually use Lzma2 default
        comp = Comp::Lzma2;
    } else if (std::memcmp(footer, "APTGEU01", 8) != 0) {
        err = "payload not found (bad magic)";
        ::CloseHandle(h);
        return false;
    }
    uint64_t payload_start = rd_u64(footer + 8);
    uint64_t footer_size = v48 ? 48 : (v16 ? 28 : 16);
    if (payload_start + footer_size > (uint64_t)sz.QuadPart) {
        err = "payload_start out of range";
        ::CloseHandle(h);
        return false;
    }
    uint64_t own_payload_size = (uint64_t)sz.QuadPart - footer_size - payload_start;
    uint64_t payload_size = v16 ? total_payload_size : own_payload_size;

    // Read whole payload into memory (may span multiple files).
    std::vector<uint8_t> buf(payload_size);
    size_t done = 0;
    // Read from own file first
    {
        LARGE_INTEGER poff; poff.QuadPart = (LONGLONG)payload_start;
        ::SetFilePointerEx(h, poff, nullptr, FILE_BEGIN);
        uint64_t to_read = own_payload_size;
        if (to_read > payload_size) to_read = payload_size;
        while (done < to_read) {
            DWORD chunk = 1 << 16;
            if ((uint64_t)chunk > to_read - done) chunk = (DWORD)(to_read - done);
            DWORD r = 0;
            if (!::ReadFile(h, buf.data() + done, chunk, &r, nullptr) || r == 0) {
                err = "read payload failed (self)";
                ::CloseHandle(h);
                return false;
            }
            done += r;
        }
    }
    ::CloseHandle(h);

    // v0.16: read remaining slices from .bin files
    if (v16 && done < payload_size) {
        // self path -> directory + base name
        std::wstring dir = self;
        size_t slash = dir.find_last_of(L"\\/");
        std::wstring base = (slash != std::wstring::npos) ? dir.substr(0, slash + 1) : L"";
        std::wstring name = (slash != std::wstring::npos) ? dir.substr(slash + 1) : dir;
        // strip .exe
        size_t dot = name.find_last_of(L".");
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        uint32_t slice = 1;
        while (done < payload_size) {
            std::wstring fname = base + name + L"-" + std::to_wstring(slice) + L".bin";
            HANDLE h2 = ::CreateFileW(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h2 == INVALID_HANDLE_VALUE) {
                err = "missing slice: " + std::string(fname.begin(), fname.end());
                return false;
            }
            // read entire file (minus its own footer)
            LARGE_INTEGER sz2; GetFileSizeEx(h2, &sz2);
            uint64_t content = sz2.QuadPart - 28; // each slice has a 28-byte footer
            while (done < payload_size && content > 0) {
                DWORD chunk = 1 << 16;
                if ((uint64_t)chunk > content) chunk = (DWORD)content;
                if ((uint64_t)chunk > payload_size - done) chunk = (DWORD)(payload_size - done);
                DWORD r = 0;
                if (!::ReadFile(h2, buf.data() + done, chunk, &r, nullptr) || r == 0) break;
                done += r;
                content -= r;
            }
            ::CloseHandle(h2);
            slice++;
        }
    }

    // v0.2.5 / M-01 / M-02: AES-256-CBC decrypt.
    //   v48 (footer "APTGEU09", post-M-01):
    //     inner_magic[8] || salt[16] || IV[16] ||
    //       AES-256-CBC(PBKDF2-HMAC-SHA256(pw,salt,100000), IV, "APTGEUSIG"[8] || compressed)
    //     MAC = HMAC-SHA256 over the whole inner envelope, verified BEFORE decrypt.
    //   v07 legacy (footer "APTGEU07"):
    //     inner_magic[8] || IV[16] || AES-256-CBC(SHA256(pw), IV, ...)
    //     (no salt, no MAC; legacy single-round KDF, logs a WARN).
    if (comp == Comp::Encrypted) {
        const size_t hdr_min = v48 ? (8 + 16 + 16 + 16) : (8 + 16 + 16);
        if (buf.size() < hdr_min) {
            err = "payload too small for encrypted header";
            return false;
        }
        if (g_password.empty()) {
            err = "密码错误";
            return false;
        }

        uint8_t inner_magic[8];
        std::memcpy(inner_magic, buf.data(), 8);
        size_t pos = 8;

        uint8_t salt[16] = {0};
        if (v48) {
            // M-01: per-package random salt, input to PBKDF2.
            std::memcpy(salt, buf.data() + pos, 16);
            pos += 16;

            // M-02: encrypt-then-MAC. Verify HMAC-SHA256 over the WHOLE inner
            // envelope (inner_magic||salt||IV||ct) BEFORE touching the ciphertext.
            uint8_t mac_key[32];
            atu::derive_hmac_key(g_password, salt, mac_key);
            bool mac_ok = atu::hmac_sha256_verify(mac_key, buf.data(), buf.size(), stored_mac);
            ::SecureZeroMemory(mac_key, sizeof(mac_key));
            if (!mac_ok) {
                g_payload_tampered = true;
                atu::log_error("encrypted payload HMAC mismatch (tampered bytes or wrong password)");
                err = "数据被篡改";
                return false;
            }
        }

        uint8_t iv[16];
        std::memcpy(iv, buf.data() + pos, 16);
        pos += 16;
        const uint8_t* cipher = buf.data() + pos;
        size_t cipher_len = buf.size() - pos;

        uint8_t key[32];
        if (v48) {
            atu::derive_aes256_key(g_password, salt, key);   // PBKDF2-HMAC-SHA256, 100000 iters
        } else {
            atu::derive_aes256_key_legacy(g_password, key);  // old APTGEU07: single SHA-256 + WARN
        }

        std::vector<uint8_t> plaintext;
        if (!atu::aes256_cbc_decrypt(key, iv, cipher, cipher_len, plaintext)) {
            ::SecureZeroMemory(key, sizeof(key));
            err = "密码错误";
            return false;
        }
        ::SecureZeroMemory(key, sizeof(key));
        // Verify the 8-byte signature prefix.
        if (plaintext.size() < 8 || std::memcmp(plaintext.data(), "APTGEUSIG", 8) != 0) {
            err = "密码错误";
            return false;
        }
        // Strip the signature; remaining bytes are the compressed payload.
        plaintext.erase(plaintext.begin(), plaintext.begin() + 8);
        buf.swap(plaintext);
        payload_size = buf.size();

        // Determine inner codec from inner_magic.
        if (std::memcmp(inner_magic, "APTGEU08", 8) == 0)      comp = Comp::Zstd;
        else if (std::memcmp(inner_magic, "APTGEU04", 8) == 0) comp = Comp::SE;
        else if (std::memcmp(inner_magic, "APTGEU02", 8) == 0) comp = Comp::Zlib;
        else if (std::memcmp(inner_magic, "APTGEU01", 8) == 0) comp = Comp::None;
        else                                                   comp = Comp::Lzma2;  // default APTGEU03
    }

    // If compressed, inflate.
    if (comp == Comp::Zlib) {
        std::vector<uint8_t> raw;
        raw.resize(payload_size * 4);
        uLongf dest_len = (uLongf)raw.size();
        int rc = ::uncompress(raw.data(), &dest_len, buf.data(), (uLong)buf.size());
        if (rc == Z_BUF_ERROR) {
            raw.resize(payload_size * 16);
            dest_len = (uLongf)raw.size();
            rc = ::uncompress(raw.data(), &dest_len, buf.data(), (uLong)buf.size());
        }
        if (rc != Z_OK) {
            err = "zlib inflate failed (rc=" + std::to_string(rc) + ")";
            return false;
        }
        raw.resize(dest_len);
        return parse_payload(raw.data(), raw.size(), out, err);
    }

    if (comp == Comp::Zstd) {
        // v0.23: raw zstd frame. The frame header carries the content size,
        // so we ask zstd for it and allocate exactly that.
        unsigned long long contentSize = ZSTD_getFrameContentSize(buf.data(), buf.size());
        if (contentSize == ZSTD_CONTENTSIZE_ERROR || contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            err = "zstd: cannot determine decompressed size";
            return false;
        }
        std::vector<uint8_t> raw((size_t)contentSize);
        size_t rc = ZSTD_decompress(raw.data(), raw.size(), buf.data(), buf.size());
        if (ZSTD_isError(rc)) {
            err = std::string("zstd decompress failed: ") + ZSTD_getErrorName(rc);
            return false;
        }
        if (rc != (size_t)contentSize) {
            raw.resize(rc);
        }
        return parse_payload(raw.data(), raw.size(), out, err);
    }

    if (comp == Comp::Lzma2) {
        // Compiler writes: byte[1] prop, uint64_le uncompressed_size, byte[] stream
        if (buf.size() < 9) { err = "LZMA2 payload too small"; return false; }
        Byte prop = buf[0];
        uint64_t uncomp_size = 0;
        for (int i = 0; i < 8; ++i)
            uncomp_size |= ((uint64_t)buf[1 + i]) << (8 * i);
        std::vector<uint8_t> raw(uncomp_size);
        SizeT dest_len = (SizeT)raw.size();
        SizeT src_len = buf.size() - 9;
        ELzmaStatus status;
        SRes rc = Lzma2Decode(raw.data(), &dest_len,
                              buf.data() + 9, &src_len,
                              prop, LZMA_FINISH_END, &status, &g_lzma_alloc);
        if (rc != SZ_OK || status != LZMA_STATUS_FINISHED_WITH_MARK) {
            char b[128];
            std::snprintf(b, sizeof(b), "LZMA2 decode failed rc=%d status=%d", (int)rc, (int)status);
            err = b;
            return false;
        }
        raw.resize(dest_len);
        return parse_payload(raw.data(), raw.size(), out, err);
    }

    if (comp == Comp::SE) {
        // SE: header has orig_size at offset 4.
        if (buf.size() < 8) { err = "SE payload too small"; return false; }
        uint32_t orig = *(const uint32_t*)(buf.data() + 4);
        std::vector<uint8_t> raw(orig);
        size_t got = se_decompress(buf.data(), buf.size(), raw.data(), raw.size());
        if (got != orig) {
            err = "SE decompress failed";
            return false;
        }
        return parse_payload(raw.data(), raw.size(), out, err);
    }

    return parse_payload(buf.data(), buf.size(), out, err);
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool ensure_dir_recursive(const std::wstring& path) {
    if (path.empty()) return false;
    // Normalize: strip trailing backslash.
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();

    // Try to create leaf; on failure recurse into parent.
    DWORD attr = ::GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // Find parent.
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0) {
        if (!ensure_dir_recursive(p.substr(0, slash))) return false;
    }
    if (!::CreateDirectoryW(p.c_str(), nullptr)) {
        // May already exist due to race; re-check.
        attr = ::GetFileAttributesW(p.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
    }
    return true;
}

static std::wstring join_path(const std::wstring& dir, const std::wstring& rel) {
    if (dir.empty()) return rel;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + rel;
    return dir + L"\\" + rel;
}

// H-01 (CWE-22 path traversal): resolve <install_dir>\<rel> and guarantee the
// result stays inside install_dir. Layered defence:
//   1. reject absolute rel paths (drive letter "X:\", UNC "\\server\share",
//      leading "/" or "\");
//   2. reject any ".." path segment in rel (covers "..\..", "foo\..\bar");
//   3. join, then normalize the result with GetFullPathNameW;
//   4. require the normalized result to be rooted inside the normalized
//      install dir on a true path-segment boundary (so "C:\App2" cannot be
//      mistaken for a child of "C:\App").
// On any failure returns false and fills err; out_abs receives the sanitized
// absolute path only on success.
static bool resolve_safe_install_path(const std::wstring& install_dir,
                                      const std::wstring& rel,
                                      std::wstring& out_abs,
                                      std::string& err) {
    // 1. Absolute rel paths are never allowed inside the install tree.
    if (rel.size() >= 2 &&
        ((rel[0] >= L'A' && rel[0] <= L'Z') || (rel[0] >= L'a' && rel[0] <= L'z')) &&
        rel[1] == L':') {
        err = std::string("blocked path traversal (drive letter in rel_path): ") +
              wide_to_utf8(rel);
        return false;
    }
    if (rel.rfind(L"\\\\", 0) == 0) {  // UNC: begins with "\\"
        err = std::string("blocked path traversal (UNC rel_path): ") +
              wide_to_utf8(rel);
        return false;
    }
    if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) {
        err = std::string("blocked path traversal (absolute rel_path): ") +
              wide_to_utf8(rel);
        return false;
    }
    // 2. Reject any ".." component.
    for (size_t pos = 0; pos <= rel.size(); ) {
        size_t slash = rel.find(L'\\', pos);
        std::wstring comp = rel.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
        if (comp == L"..") {
            err = std::string("blocked path traversal ('..' in rel_path): ") +
                  wide_to_utf8(rel);
            return false;
        }
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }
    // 3. Join + lexical normalization (also collapses stray "." / separators).
    std::wstring joined = join_path(install_dir, rel);
    wchar_t full_buf[MAX_PATH * 4] = {0};
    DWORD n = ::GetFullPathNameW(joined.c_str(), _countof(full_buf), full_buf, nullptr);
    if (n == 0 || n >= _countof(full_buf)) {
        err = std::string("GetFullPathNameW failed for rel_path: ") + wide_to_utf8(rel);
        return false;
    }
    std::wstring norm_abs(full_buf, n);

    // 4. Normalize install dir and require a bounded, boundary-respecting prefix.
    wchar_t dir_buf[MAX_PATH * 4] = {0};
    DWORD nd = ::GetFullPathNameW(install_dir.c_str(), _countof(dir_buf), dir_buf, nullptr);
    if (nd == 0 || nd >= _countof(dir_buf)) {
        err = "GetFullPathNameW failed for install_dir";
        return false;
    }
    std::wstring norm_dir(dir_buf, nd);
    while (norm_dir.size() > 1 && norm_dir.back() == L'\\') norm_dir.pop_back();

    if (_wcsnicmp(norm_abs.c_str(), norm_dir.c_str(), norm_dir.size()) != 0) {
        err = std::string("blocked path traversal (escapes install_dir): ") +
              wide_to_utf8(norm_abs);
        return false;
    }
    if (norm_abs.size() > norm_dir.size() && norm_abs[norm_dir.size()] != L'\\') {
        err = std::string("blocked path traversal (prefix collision): ") +
              wide_to_utf8(norm_abs);
        return false;
    }
    out_abs = norm_abs;
    return true;
}

bool install_files(const ParsedPayload& p,
                   const std::wstring& install_dir,
                   std::vector<std::wstring>& written_paths,
                   std::string& err,
                   const std::set<std::string>* selected_components /*=nullptr*/,
                   const std::set<std::string>* selected_tasks /*=nullptr*/) {
    written_paths.clear();
    if (!ensure_dir_recursive(install_dir)) {
        err = "cannot create install dir";
        return false;
    }

    for (const auto& f : p.files) {
        // v0.4: skip files whose component is not selected.
        // component.empty() = always installed.
        if (selected_components && !f.component.empty() &&
            selected_components->find(f.component) == selected_components->end()) {
            continue;
        }
        // v0.14: skip files whose task is not selected.
        if (selected_tasks && !f.tasks.empty() &&
            selected_tasks->find(f.tasks) == selected_tasks->end()) {
            continue;
        }
        // rel_path is UTF-8 with '/'. Convert to wide with '\'.
        std::wstring rel_w = utf8_to_wide(f.rel_path);
        for (auto& c : rel_w) if (c == L'/') c = L'\\';

        // H-01: reject traversal rel paths and resolve a sanitized absolute
        // path that is guaranteed to stay inside install_dir.
        std::wstring abs;
        if (!resolve_safe_install_path(install_dir, rel_w, abs, err)) {
            return false;
        }

        // v0.19: onlyifdoesntexist - skip if file already exists
        if (f.flags.find("onlyifdoesntexist") != std::string::npos) {
            DWORD attr = ::GetFileAttributesW(abs.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) continue;
        }
        // v0.19: uninsneveruninstall - mark file to not be deleted on uninstall
        bool no_uninstall = f.flags.find("uninsneveruninstall") != std::string::npos;

        // Ensure parent dir exists.
        size_t slash = abs.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            if (!ensure_dir_recursive(abs.substr(0, slash))) {
                err = "cannot create subdir for " + f.rel_path;
                return false;
            }
        }

        HANDLE h = ::CreateFileW(abs.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = "CreateFileW write failed: " + f.rel_path;
            return false;
        }
        DWORD wrote = 0;
        size_t off = 0;
        bool ok = true;
        while (off < f.data.size()) {
            DWORD chunk = 1 << 16;
            if ((size_t)chunk > f.data.size() - off) chunk = (DWORD)(f.data.size() - off);
            DWORD w = 0;
            if (!::WriteFile(h, f.data.data() + off, chunk, &w, nullptr) || w == 0) {
                ok = false; break;
            }
            off += w;
            wrote += w;
        }
        ::CloseHandle(h);
        if (!ok || wrote != f.data.size()) {
            err = "WriteFile failed: " + f.rel_path;
            return false;
        }

        // Re-read and verify CRC (cheap safety net; data is already in memory,
        // but this guards against partial writes / FS issues).
        uint32_t actual = atu::crc32(f.data.data(), f.data.size());
        if (actual != f.crc32) {
            err = "post-write CRC mismatch: " + f.rel_path;
            return false;
        }

        // v0.2.2: re-open the on-disk file and verify its MD5 matches the
        // digest recorded in the payload. This catches bit-rot / tampering
        // of the installed file, not just in-memory corruption.
        if (f.has_md5) {
            HANDLE hf = ::CreateFileW(abs.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
            if (hf == INVALID_HANDLE_VALUE) {
                err = "无法打开已安装文件进行 MD5 校验: " + f.rel_path;
                return false;
            }
            md5_ctx mc;
            md5_init(&mc);
            uint8_t buf[64 * 1024];
            DWORD got = 0;
            while (::ReadFile(hf, buf, sizeof(buf), &got, nullptr) && got > 0) {
                md5_update(&mc, buf, got);
            }
            ::CloseHandle(hf);
            unsigned char digest[16];
            md5_final(&mc, digest);
            if (std::memcmp(digest, f.md5, 16) != 0) {
                err = "文件完整性校验失败: " + f.rel_path;
                return false;
            }
        }

        // v0.19: deleteafterinstall - delete the installed file after writing
        // (used for temp/setup-only files)
        if (f.flags.find("deleteafterinstall") != std::string::npos) {
            ::DeleteFileW(abs.c_str());
        }

        written_paths.push_back(abs);
    }
    return true;
}

bool write_unins_dat(const std::wstring& install_dir,
                     const std::vector<std::wstring>& written_paths) {
    std::wstring dat = join_path(install_dir, L"unins000.dat");
    HANDLE h = ::CreateFileW(dat.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (const auto& p : written_paths) {
        std::string u8 = wide_to_utf8(p);
        u8 += "\r\n";
        DWORD w = 0;
        if (!::WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr) || w != u8.size()) {
            ok = false; break;
        }
    }
    ::CloseHandle(h);
    return ok;
}

// Write the standalone uninstaller PE (from APTGEU::unins.exe payload entry)
// to installDir\unins000.exe. NOT added to written_paths (it deletes itself).
bool install_unins_exe(const std::vector<uint8_t>& data,
                      const std::wstring& install_dir) {
    if (data.empty()) return false;
    std::wstring abs = join_path(install_dir, L"unins000.exe");
    ::DeleteFileW(abs.c_str());
    HANDLE h = ::CreateFileW(abs.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = 1 << 16;
        if ((size_t)chunk > data.size() - off) chunk = (DWORD)(data.size() - off);
        DWORD w = 0;
        if (!::WriteFile(h, data.data() + off, chunk, &w, nullptr) || w == 0) {
            ok = false; break;
        }
        off += w;
        wrote += w;
    }
    ::CloseHandle(h);
    return ok && wrote == data.size();
}

bool install_unins_ini(const std::vector<uint8_t>& data,
                       const std::wstring& install_dir) {
    if (data.empty()) return false;
    std::wstring abs = join_path(install_dir, L"unins000.ini");
    ::DeleteFileW(abs.c_str());
    HANDLE h = ::CreateFileW(abs.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = 1 << 16;
        if ((size_t)chunk > data.size() - off) chunk = (DWORD)(data.size() - off);
        DWORD w = 0;
        if (!::WriteFile(h, data.data() + off, chunk, &w, nullptr) || w == 0) { ok = false; break; }
        off += w; wrote += w;
    }
    ::CloseHandle(h);
    return ok && wrote == data.size();
}

bool copy_self_to(const std::wstring& dest_path) {
    std::wstring self = get_own_exe_path();
    if (self.empty()) return false;
    // CopyFileW fails if dest exists; delete first.
    ::DeleteFileW(dest_path.c_str());
    return ::CopyFileW(self.c_str(), dest_path.c_str(), FALSE) != 0;
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------

int uninstall_delete_listed_files(const std::wstring& dat_path) {
    HANDLE h = ::CreateFileW(dat_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    std::string content;
    char buf[4096];
    DWORD r = 0;
    while (::ReadFile(h, buf, sizeof(buf), &r, nullptr) && r > 0) {
        content.append(buf, r);
    }
    ::CloseHandle(h);

    int deleted = 0;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::wstring w = utf8_to_wide(line);
        if (::DeleteFileW(w.c_str())) ++deleted;
    }
    return deleted;
}

void uninstall_remove_tree(const std::wstring& dir) {
    // Best-effort recursive removal. Skip files we cannot delete (e.g. the
    // running unins000.exe) - those are handled by schedule_self_delete.
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            uninstall_remove_tree(full);
            ::RemoveDirectoryW(full.c_str());
        } else {
            ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(full.c_str());
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    ::RemoveDirectoryW(dir.c_str());
}

void schedule_self_delete(const std::wstring& self_path,
                          const std::wstring& install_dir) {
    // Build: cmd /c ping 127.0.0.1 -n 3 >NUL & del /f /q "self" & rd /s /q "dir"
    std::wstring cmd = L"cmd /c ping 127.0.0.1 -n 3 >NUL & del /f /q \"";
    cmd += self_path;
    cmd += L"\" & rd /s /q \"";
    cmd += install_dir;
    cmd += L"\"";

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(L'\0');
    if (::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | DETACHED_PROCESS,
                         nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------
// Shortcuts (IShellLink COM)
// ---------------------------------------------------------------------------

static void dbg_log(const std::wstring& msg) {
    wchar_t buf[MAX_PATH];
    ::GetTempPathW(MAX_PATH, buf);
    std::wstring path = std::wstring(buf) + L"aptgeu_shortcut_debug.txt";
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    wchar_t hdr[64];
    wsprintfW(hdr, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(hdr) + msg + L"\r\n";
    DWORD w = 0;
    ::WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &w, nullptr);
    ::CloseHandle(h);
}

std::vector<std::wstring> create_shortcuts(const std::wstring& install_dir,
                                           const std::wstring& app_name,
                                           const std::wstring& exe_relative_path,
                                           const std::wstring& group_name,
                                           bool create_desktop,
                                           bool create_progmenu,
                                           bool create_quicklaunch) {
    std::vector<std::wstring> created;

    std::wstring exe_path = install_dir + L"\\" + exe_relative_path;
    dbg_log(std::wstring(L"create_shortcuts: install_dir=") + install_dir + L" app_name=" + app_name + L" exe=" + exe_path + L" group=" + group_name
            + L" desktop=" + (create_desktop?L"1":L"0")
            + L" progmenu=" + (create_progmenu?L"1":L"0")
            + L" quicklaunch=" + (create_quicklaunch?L"1":L"0"));

    // COM init (may already be init'd by caller; ignore RPC_E_CHANGED_MODE).
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(hr);
    dbg_log(std::wstring(L"CoInitializeEx hr=0x") + std::to_wstring((unsigned long)hr) + L" com_inited=" + (com_inited?L"1":L"0"));

    // Resolve special folders.
    auto get_folder = [](int csidl) -> std::wstring {
        wchar_t path[MAX_PATH] = {0};
        if (::SHGetFolderPathW(nullptr, csidl, nullptr, 0, path) == S_OK)
            return path;
        return std::wstring();
    };

    std::wstring start_menu = get_folder(CSIDL_STARTMENU);
    std::wstring programs_dir = start_menu + L"\\Programs";
    std::wstring desktop = get_folder(CSIDL_DESKTOPDIRECTORY);
    dbg_log(L"start_menu=" + start_menu);
    dbg_log(L"programs_dir=" + programs_dir);
    dbg_log(L"desktop=" + desktop);

    // Inno /GROUP=...: place the Start Menu shortcut in a named sub-folder.
    std::wstring start_menu_dir = programs_dir;
    if (!group_name.empty()) {
        std::wstring gn = group_name;
        while (!gn.empty() && (gn.front() == L'"' || gn.front() == L' ')) gn.erase(0, 1);
        while (!gn.empty() && (gn.back() == L'"' || gn.back() == L' ')) gn.pop_back();
        if (!gn.empty()) start_menu_dir = programs_dir + L"\\" + gn;
    }

    // v0.24: build the target list from the per-task bools.
    std::vector<std::wstring> targets;
    if (create_progmenu && !start_menu_dir.empty())
        targets.push_back(start_menu_dir + L"\\" + app_name + L".lnk");
    if (create_desktop && !desktop.empty())
        targets.push_back(desktop + L"\\" + app_name + L".lnk");
    if (create_quicklaunch) {
        // Quick Launch toolbar lives under %APPDATA%\Microsoft\Internet
        // Explorer\Quick Launch. On Win10/11 the toolbar is hidden by default
        // but the folder still exists (we create it if missing); the .lnk is
        // recorded in unins000.dat so uninstall cleans it up.
        std::wstring appdata = get_folder(CSIDL_APPDATA);
        if (!appdata.empty()) {
            std::wstring ql = appdata + L"\\Microsoft\\Internet Explorer\\Quick Launch";
            targets.push_back(ql + L"\\" + app_name + L".lnk");
        }
    }

    for (const auto& lnk : targets) {
        dbg_log(L"--- target: " + lnk);
        // Ensure parent dir exists.
        size_t slash = lnk.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            bool ok = ensure_dir_recursive(lnk.substr(0, slash));
            dbg_log(std::wstring(L"ensure_dir_recursive ok=") + (ok?L"1":L"0"));
        }

        IShellLinkW* sl = nullptr;
        hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl);
        dbg_log(std::wstring(L"CoCreateInstance hr=0x") + std::to_wstring((unsigned long)hr) + L" sl=" + (sl?L"ok":L"null"));
        if (FAILED(hr) || !sl) continue;

        sl->SetPath(exe_path.c_str());
        sl->SetWorkingDirectory(install_dir.c_str());
        sl->SetIconLocation(exe_path.c_str(), 0);
        sl->SetDescription(app_name.c_str());

        IPersistFile* pf = nullptr;
        hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
        dbg_log(std::wstring(L"QueryInterface IPersistFile hr=0x") + std::to_wstring((unsigned long)hr) + L" pf=" + (pf?L"ok":L"null"));
        if (SUCCEEDED(hr) && pf) {
            hr = pf->Save(lnk.c_str(), TRUE);
            dbg_log(std::wstring(L"IPersistFile::Save hr=0x") + std::to_wstring((unsigned long)hr));
            pf->Release();
            if (SUCCEEDED(hr)) created.push_back(lnk);
        }
        sl->Release();
    }

    dbg_log(std::wstring(L"created ") + std::to_wstring((unsigned long)created.size()) + L" shortcuts");
    if (com_inited) ::CoUninitialize();
    return created;
}

} // namespace stub
