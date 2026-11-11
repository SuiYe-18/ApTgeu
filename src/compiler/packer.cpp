/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "packer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <sys/stat.h>
#include <direct.h>
#include <windows.h>
#include <wininet.h>
#include "Lzma2Enc.h"
#include "se.h"
#include "zlib.h"
#include "zstd.h"
#include "md5.h"
#include "crypto.h"

namespace atu {

namespace {

// Simple allocator for LZMA (malloc/free wrappers)
void* lzma_alloc_func(const ISzAlloc*, size_t size) { return malloc(size); }
void lzma_free_func(const ISzAlloc*, void* p) { free(p); }
ISzAlloc g_Alloc = { lzma_alloc_func, lzma_free_func };

// v0.4: LZMA2-compress a buffer. Output layout (for stub):
//   byte   props       (1 byte, from Lzma2Enc_WriteProperties)
//   uint64 uncompressed_size (LE)
//   byte[] lzma2_stream
// Throws std::runtime_error on failure.
std::vector<uint8_t> lzma2_compress(const std::vector<uint8_t>& raw, int level, uint32_t dictSize) {
    CLzma2EncHandle enc = Lzma2Enc_Create(&g_Alloc, &g_Alloc);
    if (!enc) throw std::runtime_error("Lzma2Enc_Create failed");

    CLzma2EncProps props;
    Lzma2EncProps_Init(&props);
    props.lzmaProps.level = level;
    props.lzmaProps.dictSize = dictSize;
    props.lzmaProps.fb = 273;
    props.lzmaProps.btMode = 1;
    props.lzmaProps.numHashBytes = 4;
    props.lzmaProps.mc = 1 << 20;
    props.lzmaProps.writeEndMark = 1;
    props.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
    props.numTotalThreads = 1;
    Lzma2EncProps_Normalize(&props);

    SRes rc = Lzma2Enc_SetProps(enc, &props);
    if (rc != SZ_OK) {
        Lzma2Enc_Destroy(enc);
        throw std::runtime_error("Lzma2Enc_SetProps failed: " + std::to_string(rc));
    }
    Byte prop = Lzma2Enc_WriteProperties(enc);
    Lzma2Enc_SetDataSize(enc, raw.size());

    // Output bound: raw.size() + 16 bytes slack for LZMA2 overhead.
    size_t out_cap = raw.size() + (raw.size() / 2) + 64;
    std::vector<uint8_t> out_buf(out_cap);
    size_t out_len = out_cap;
    rc = Lzma2Enc_Encode2(enc,
                          nullptr,                 // outStream
                          out_buf.data(), &out_len, // outBuf
                          nullptr,                 // inStream
                          raw.data(), raw.size(),   // inData
                          nullptr);                 // progress
    Lzma2Enc_Destroy(enc);
    if (rc != SZ_OK) throw std::runtime_error("Lzma2Enc_Encode2 failed: " + std::to_string(rc));

    // Prepend props byte + uncompressed size
    std::vector<uint8_t> out;
    out.reserve(1 + 8 + out_len);
    out.push_back(prop);
    uint64_t sz = raw.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((sz >> (8*i)) & 0xFF));
    out.insert(out.end(), out_buf.begin(), out_buf.begin() + out_len);
    return out;
}

// perf: zlib (deflate, zlib-wrapped) compression. Stub inflates via uncompress().
std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& raw) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("deflateInit failed");
    size_t cap = raw.size() + raw.size()/1000 + 64;
    std::vector<uint8_t> out(cap);
    strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    strm.avail_in = (uInt)raw.size();
    strm.next_out = out.data();
    strm.avail_out = (uInt)out.size();
    int rc = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);
    if (rc != Z_STREAM_END) throw std::runtime_error("deflate failed: " + std::to_string(rc));
    out.resize(strm.total_out);
    return out;
}

// v0.23: zstd (Meta/Facebook) compression. Stub inflates via ZSTD_decompress.
// Wire format = raw zstd frame (frame header already carries content size,
// so no extra header is needed). Level 3 is a good speed/ratio default.
std::vector<uint8_t> zstd_compress(const std::vector<uint8_t>& raw, int level) {
    size_t bound = ZSTD_compressBound(raw.size());
    std::vector<uint8_t> out(bound);
    size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(n)) {
        throw std::runtime_error(std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
}

std::vector<uint8_t> read_file_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::string read_file_text(const std::string& path) {
    auto b = read_file_binary(path);
    return std::string(b.begin(), b.end());
}

// v0.2.5: download a file from http:// or https:// via WinINet.
// Throws std::runtime_error("下载失败: <url>") on any failure.
// Prints a console progress bar as bytes arrive.
std::vector<uint8_t> download_url(const std::string& url) {
    std::wstring wurl(url.begin(), url.end());
    HINTERNET hSession = ::InternetOpenW(L"ApTgeuCompiler/0.2.5",
                                          INTERNET_OPEN_TYPE_PRECONFIG,
                                          nullptr, nullptr, 0);
    if (!hSession) {
        throw std::runtime_error("下载失败: " + url);
    }
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI;
    HINTERNET hUrl = ::InternetOpenUrlW(hSession, wurl.c_str(), nullptr, 0, flags, 0);
    if (!hUrl) {
        ::InternetCloseHandle(hSession);
        throw std::runtime_error("下载失败: " + url);
    }

    // Try to get Content-Length for the progress bar.
    DWORD content_len = 0;
    DWORD len_buf = sizeof(content_len);
    DWORD index = 0;
    ::HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                     &content_len, &len_buf, &index);

    std::vector<uint8_t> data;
    uint8_t chunk[64 * 1024];
    DWORD got = 0;
    uint64_t total = 0;
    bool ok = true;
    while (::InternetReadFile(hUrl, chunk, sizeof(chunk), &got) && got > 0) {
        data.insert(data.end(), chunk, chunk + got);
        total += got;
        if (content_len > 0) {
            int pct = (int)(total * 100 / content_len);
            int bar_w = 30;
            int filled = pct * bar_w / 100;
            std::printf("\r[下载] %s  [", url.c_str());
            for (int i = 0; i < bar_w; ++i) std::putchar(i < filled ? '#' : '-');
            std::printf("] %3d%%  %llu/%llu  ", pct,
                        (unsigned long long)total, (unsigned long long)content_len);
            std::fflush(stdout);
        } else {
            std::printf("\r[下载] %s  %llu 字节   ", url.c_str(),
                        (unsigned long long)total);
            std::fflush(stdout);
        }
    }
    std::printf("\n");

    ::InternetCloseHandle(hUrl);
    ::InternetCloseHandle(hSession);

    if (data.empty()) {
        throw std::runtime_error("下载失败: " + url);
    }
    return data;
}

bool is_http_url(const std::string& s) {
    return (s.size() > 7 && s.substr(0, 7) == "http://") ||
           (s.size() > 8 && s.substr(0, 8) == "https://");
}

bool file_exists(const std::string& p) {
    struct _stat st;
    return ::_stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFMT) != _S_IFDIR;
}

bool dir_exists(const std::string& p) {
    struct _stat st;
    return ::_stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFDIR;
}

void make_dirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            std::string sub = path.substr(0, i);
            if (!sub.empty() && !dir_exists(sub)) {
                _mkdir(sub.c_str());
            }
        }
    }
}

std::string to_forward(const std::string& s) {
    std::string r = s;
    for (auto& c : r) if (c == '\\') c = '/';
    return r;
}

std::string basename(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string parent_dir(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string(".") : p.substr(0, i);
}

std::string compute_rel_path(const std::string& base_dir, const std::string& target_dir, const std::string& src_path) {
    std::string b = to_forward(base_dir);
    std::string t = to_forward(target_dir);
    while (!b.empty() && b.back() == '/') b.pop_back();
    while (!t.empty() && t.back() == '/') t.pop_back();

    std::string rel;
    if (t.size() > b.size() && t.compare(0, b.size(), b) == 0 && t[b.size()] == '/') {
        rel = t.substr(b.size() + 1);
    } else if (t == b) {
        rel = "";
    } else {
        throw std::runtime_error("DestDir is not under DefaultDirName: '" + target_dir + "' vs '" + base_dir + "'");
    }
    std::string base = basename(src_path);
    if (rel.empty()) return base;
    return rel + "/" + base;
}

// Resolve possibly-relative path against a base directory.
std::string resolve_against(const std::string& p, const std::string& base_dir) {
    if (p.empty()) return p;
    bool abs = false;
    if (p.size() >= 2 && p[1] == ':') abs = true;
    if (p.size() >= 1 && (p[0] == '/' || p[0] == '\\')) abs = true;
    if (abs) return p;
    return base_dir + "\\" + p;
}

} // namespace

std::string own_exe_dir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string full(buf);
    size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? "." : full.substr(0, slash);
}

PackResult pack(const std::string& atu_path,
                const std::string& stub_path,
                const std::string& ui_override) {
    // 1. Parse script
    AtuScript scr = parse_atu_file(atu_path);
    Resolved r = resolve_script(scr);

    std::string atu_dir = parent_dir(atu_path);

    // 2. Load UI theme (optional, never fatal)
    AtuiTheme theme;
    bool theme_loaded = false;
    std::string ui_path = ui_override.empty() ? scr.get("UiFile", "") : ui_override;
    if (!ui_path.empty()) {
        std::string full_ui = resolve_against(ui_path, atu_dir);
        theme_loaded = ParseAtuiFile(full_ui, parent_dir(full_ui), theme);
        if (!theme_loaded) {
            // .atui missing -> fall back to defaults, do not error out.
        }
    }

    // 3. Read stub
    if (!file_exists(stub_path)) {
        throw std::runtime_error("找不到 stub.exe: " + stub_path +
            "  (请先编译 build\\stub.exe，或把 stub.exe 放到 aptgeu.exe 同目录)");
    }
    std::vector<uint8_t> stub = read_file_binary(stub_path);

    // 4. Build payload file list
    // v0.4: if [Components] section exists, default component is "main".
    bool has_components = !scr.components.empty();
    // v0.14: inject registry entries into setup config
    for (size_t i = 0; i < scr.reg_entries.size(); ++i) {
        const auto& re = scr.reg_entries[i];
        std::string val = re.root + "|" + re.subkey + "|" + re.valuetype + "|" + re.valuename + "|" + re.valuedata;
        std::string key = "Reg" + std::to_string(i);
        scr.setup[key] = val;
    }
    if (!scr.reg_entries.empty()) scr.setup["RegCount"] = std::to_string(scr.reg_entries.size());
    // v0.15: INI entries
    for (size_t i = 0; i < scr.ini_entries.size(); ++i) {
        const auto& ie = scr.ini_entries[i];
        scr.setup["Ini" + std::to_string(i)] = ie.filename + "|" + ie.section + "|"
                                              + ie.key + "|" + ie.value + "|" + ie.flags;
    }
    if (!scr.ini_entries.empty()) scr.setup["IniCount"] = std::to_string(scr.ini_entries.size());
    // v0.15: InstallDelete
    for (size_t i = 0; i < scr.install_delete.size(); ++i) {
        scr.setup["InstDel" + std::to_string(i)] = scr.install_delete[i].type + "|" + scr.install_delete[i].name;
    }
    if (!scr.install_delete.empty()) scr.setup["InstDelCount"] = std::to_string(scr.install_delete.size());
    // v0.15/v0.24: UninstallDelete.
    // [UninstallRun] entries are passed directly to build_payload() below
    // (they were previously stuffed into scr.setup, which build_payload
    // never serializes - that was the bug that made [UninstallRun] a no-op).
    for (size_t i = 0; i < scr.unins_delete.size(); ++i) {
        scr.setup["UninsDel" + std::to_string(i)] = scr.unins_delete[i].type + "|" + scr.unins_delete[i].name;
    }
    if (!scr.unins_delete.empty()) scr.setup["UninsDelCount"] = std::to_string(scr.unins_delete.size());
    // v0.24: [Run] entries run at install time (after files are laid down).
    // Wire: InstRun{i} = filename|parameters|workingdir|flags|description
    // {app} is left literal; the stub substitutes install_dir at runtime.
    for (size_t i = 0; i < scr.run_entries.size(); ++i) {
        const auto& re = scr.run_entries[i];
        scr.setup["InstRun" + std::to_string(i)] =
            re.filename + "|" + re.parameters + "|" + re.workingdir + "|" +
            re.flags + "|" + re.description;
    }
    if (!scr.run_entries.empty()) scr.setup["InstRunCount"] = std::to_string(scr.run_entries.size());
    // v0.16: Icons
    for (size_t i = 0; i < scr.icons.size(); ++i) {
        const auto& ie = scr.icons[i];
        std::string val = ie.name + "|" + ie.filename + "|" + ie.icon_filename + "|" + ie.comment + "|" + ie.parameters + "|" + ie.workingdir;
        scr.setup["Icon" + std::to_string(i)] = val;
    }
    if (!scr.icons.empty()) scr.setup["IconCount"] = std::to_string(scr.icons.size());
    // v0.19: Dirs
    for (size_t i = 0; i < scr.dirs.size(); ++i) {
        scr.setup["Dir" + std::to_string(i)] = scr.dirs[i].name;
    }
    if (!scr.dirs.empty()) scr.setup["DirCount"] = std::to_string(scr.dirs.size());
    std::vector<PayloadFile> files;
    for (const auto& fe : scr.files) {
        std::string src_path = substitute(fe.source, r);
        std::string destdir  = substitute(fe.destdir, r);
        PayloadFile pf;
        if (is_http_url(src_path)) {
            // v0.2.5: download from http(s) at compile time and embed bytes.
            std::printf("[pack] 下载远程文件: %s\n", src_path.c_str());
            pf.data = download_url(src_path);
            // rel_path = basename of URL (compute_rel_path uses basename(src_path)).
            // We still need destdir resolution; pass the URL as src_path so
            // basename() picks the last path segment.
            pf.rel_path = compute_rel_path(r.default_dir, destdir, src_path);
        } else {
            if (!file_exists(src_path)) {
                throw std::runtime_error("Source 文件不存在: " + src_path);
            }
            pf.data = read_file_binary(src_path);
            pf.rel_path = compute_rel_path(r.default_dir, destdir, src_path);
        }
        pf.component = fe.component.empty() ? (has_components ? "main" : "") : fe.component;
        pf.tasks = fe.tasks;
        pf.flags = fe.flags;
        files.push_back(std::move(pf));
    }

    // 5. Welcome image priority: .atui WelcomeBanner > .atu WelcomeImage > empty.
    //    The chosen bytes also go into the legacy APTGEU::welcome.png special entry.
    std::vector<uint8_t> welcome_bytes;
    if (!theme.welcome_banner_png.empty()) {
        welcome_bytes = theme.welcome_banner_png; // already in theme UI block
    } else if (!r.welcome_image.empty()) {
        if (!file_exists(r.welcome_image)) {
            throw std::runtime_error("WelcomeImage 文件不存在: " + r.welcome_image);
        }
        welcome_bytes = read_file_binary(r.welcome_image);
        theme.welcome_banner_png = welcome_bytes; // put it into UI block for stub
    }
    if (!welcome_bytes.empty()) {
        PayloadFile pf;
        pf.rel_path = "APTGEU::welcome.png";
        pf.data = welcome_bytes;
        files.push_back(std::move(pf));
    }

    // 6. Special entry: license.txt
    if (!r.license_file.empty()) {
        if (!file_exists(r.license_file)) {
            throw std::runtime_error("LicenseFile 文件不存在: " + r.license_file);
        }
        PayloadFile pf;
        pf.rel_path = "APTGEU::license.txt";
        std::string txt = read_file_text(r.license_file);
        pf.data.assign(txt.begin(), txt.end());
        files.push_back(std::move(pf));
    }

    // 6b. v0.2.1: embed standalone unins.exe next to aptgeu.exe (build\unins.exe).
    // If missing, warn and continue (stub will fall back to copying itself).
    {
        std::string unins_path = atu::own_exe_dir() + "\\unins.exe";
        if (file_exists(unins_path)) {
            PayloadFile pf;
            pf.rel_path = "APTGEU::unins.exe";
            pf.data = read_file_binary(unins_path);
            std::printf("[pack] 嵌入 unins.exe: %zu 字节\n", pf.data.size());
            files.push_back(std::move(pf));
        } else {
            std::fprintf(stderr, "[warn] %s 不存在，跳过嵌入（stub 将回退为复制自己）\n",
                         unins_path.c_str());
        }
    }

    // 6c. v0.12: embed uninstall UI theme as APTGEU::unins000.ini.
    if (!r.uninstall_ui_file.empty()) {
        std::string full_ini = resolve_against(r.uninstall_ui_file, atu_dir);
        if (file_exists(full_ini)) {
            PayloadFile pf;
            pf.rel_path = "APTGEU::unins000.ini";
            pf.data = read_file_binary(full_ini);
            std::printf("[pack] 嵌入 unins000.ini: %zu 字节\n", pf.data.size());
            files.push_back(std::move(pf));
        } else {
            std::fprintf(stderr, "[warn] UninstallUiFile %s 不存在，跳过\n", full_ini.c_str());
        }
    }

    // 6d. v0.22: embed VC++ 2015-2022 x64 redistributable bootstrapper.
    // Source resolution:
    //   1) [Setup] VcRedistSource="..."  -> relative to the .atu script dir
    //   2) otherwise                     -> <atu_dir>\vcredist_x64.exe
    // If the file is missing we DO NOT fail the build: the stub will detect
    // that the runtime is absent, find no payload, and log a warning.
    if (r.install_vcredist) {
        std::string src = r.vcredist_source.empty()
                            ? (atu_dir + "\\vcredist_x64.exe")
                            : resolve_against(r.vcredist_source, atu_dir);
        if (file_exists(src)) {
            PayloadFile pf;
            pf.rel_path = "APTGEU::vcredist_x64.exe";
            pf.data = read_file_binary(src);
            std::printf("[pack] 嵌入 vcredist_x64.exe: %s (%zu 字节)\n",
                        src.c_str(), pf.data.size());
            files.push_back(std::move(pf));
        } else {
            std::fprintf(stderr,
                "[warn] InstallVcRedist=yes 但找不到 vcredist 安装包: %s\n"
                "[warn]        stub 将仅检测系统是否已安装 VC++ 2015-2022 x64，不会执行安装\n",
                src.c_str());
        }
    }

    // 7. Build payload + footer. Always pass theme (even if defaults) so stub can
    //    apply a consistent theme; ui_block_size is still > 0 because we serialize
    //    the (possibly default) theme.
    const AtuiTheme* theme_ptr = (ui_path.empty() && welcome_bytes.empty()) ? nullptr : &theme;
    // Even if no UiFile and no welcome image, if the .atu had any ThemeColor we still
    // want defaults. But to keep v0.1 wire compat clean: only emit ui_block when the
    // user actually referenced a theme OR we injected a fallback welcome image.
    // 7. Build payload + footer.
    auto is_already_compressed = [](const std::vector<uint8_t>& d) -> bool {
        if (d.size() < 4) return false;
        if (d[0]==0x89 && d[1]=='P' && d[2]=='N' && d[3]=='G') return true;
        if (d[0]=='M' && d[1]=='Z') return true;
        if (d[0]==0 && d[1]==0 && d[2]==1 && d[3]==0) return true;
        if (d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF) return true;
        return false;
    };
    for (auto& f : files) f.compression = is_already_compressed(f.data) ? 0 : 1;

    // v0.2.2: per-file MD5 integrity hash. Enabled by [Setup] VerifyHash=md5.
    std::string verify_hash = scr.get("VerifyHash", "");
    bool verify_md5 = (verify_hash == "md5");
    if (verify_md5) {
        for (auto& f : files) {
            md5(f.data.data(), f.data.size(), f.md5);
        }
        std::printf("[pack] VerifyHash=md5: 已为 %zu 个文件写入 MD5\n", files.size());
    }

    std::vector<uint8_t> payload_raw = build_payload(r, files, theme_ptr,
                                                     has_components ? &scr.components : nullptr,
                                                     &scr.tasks,
                                                     verify_md5,
                                                     &scr.reg_entries,
                                                     &scr.services,
                                                     &scr.ini_entries,
                                                     &scr.unins_run,
                                                     &scr.run_entries);

    uint32_t ui_block_size = 0;
    if (theme_ptr) {
        std::vector<uint8_t> tmp;
        SerializeUiBlock(*theme_ptr, tmp);
        ui_block_size = static_cast<uint32_t>(tmp.size());
    }

    // Global compression (keeps UI block compressed).
    // Magic: 01=none, 02=zlib, 03=LZMA2, 04=SE, 07=AES-encrypted wrapper, 08=zstd.
    std::vector<uint8_t> payload;
    char footer_magic[8] = {'A','P','T','G','E','U','0','3'};
    if (r.compression == "se") {
        size_t cap = payload_raw.size() + payload_raw.size() / 8 + 64;
        std::vector<uint8_t> buf(cap);
        size_t n = se_compress(payload_raw.data(), payload_raw.size(), buf.data(), buf.size());
        if (n == 0) throw std::runtime_error("se_compress failed");
        buf.resize(n);
        payload = std::move(buf);
        memcpy(footer_magic, "APTGEU04", 8);
    } else if (r.compression == "none") {
        payload = payload_raw;
        memcpy(footer_magic, "APTGEU01", 8);
    } else if (r.compression == "zlib") {
        payload = zlib_compress(payload_raw);
        memcpy(footer_magic, "APTGEU02", 8);
    } else if (r.compression == "zstd") {
        // Map LZMA-style 1..9 level onto zstd 1..19 (default 3 if out of range).
        int zlevel = 3;
        if (r.lzma_level >= 1 && r.lzma_level <= 19) zlevel = r.lzma_level;
        std::printf("[pack] zstd compress (level=%d, raw=%zu bytes)\n", zlevel, payload_raw.size());
        payload = zstd_compress(payload_raw, zlevel);
        memcpy(footer_magic, "APTGEU08", 8);
    } else {
        payload = lzma2_compress(payload_raw, r.lzma_level, r.lzma_dict);
        memcpy(footer_magic, "APTGEU03", 8);
    }

    // v0.2.5: AES-256-CBC encrypt payload if Password set (replaces v0.16 XOR).
    // Wire layout (when encrypted):
    //   payload_out = inner_magic[8] || IV[16] || AES-256-CBC(key, IV, "APTGEUSIG"[8] || payload_compressed)
    //   footer magic = "APTGEU07"
    // key = SHA256(password). The stub reads inner_magic + IV, prompts for /PASSWORD=,
    // derives the same key, decrypts, and checks the "APTGEUSIG" prefix.
    std::string password;
    {
        auto it = scr.setup.find("Password");
        if (it != scr.setup.end()) password = it->second;
    }
    bool encrypted = false;
    // M-02: HMAC-SHA256 over the whole encrypted envelope (inner_magic||salt||IV||ct).
    // Stored in the 48-byte outer footer when encrypted == true.
    uint8_t out_mac[32] = {0};
    if (!password.empty()) {
        // M-01: per-package random 16-byte salt, fed into PBKDF2-HMAC-SHA256.
        uint8_t salt[16];
        atu::random_bytes(salt, sizeof(salt));

        uint8_t key[32];
        atu::derive_aes256_key(password, salt, key);
        uint8_t iv[16];
        atu::random_bytes(iv, sizeof(iv));

        // plaintext = "APTGEUSIG" || compressed payload
        std::vector<uint8_t> plaintext;
        plaintext.insert(plaintext.end(), "APTGEUSIG", "APTGEUSIG" + 8);
        plaintext.insert(plaintext.end(), payload.begin(), payload.end());

        std::vector<uint8_t> ciphertext;
        atu::aes256_cbc_encrypt(key, iv, plaintext.data(), plaintext.size(), ciphertext);

        // inner envelope: inner_magic(8) || salt(16) || IV(16) || ciphertext
        std::vector<uint8_t> enc_payload;
        enc_payload.insert(enc_payload.end(), footer_magic, footer_magic + 8);
        enc_payload.insert(enc_payload.end(), salt, salt + 16);
        enc_payload.insert(enc_payload.end(), iv, iv + 16);
        enc_payload.insert(enc_payload.end(), ciphertext.begin(), ciphertext.end());

        payload.swap(enc_payload);

        // Encrypt-then-MAC: authenticate inner_magic || salt || IV || ciphertext
        // with a key derived separately (domain-separated) from the AES key.
        uint8_t mac_key[32];
        atu::derive_hmac_key(password, salt, mac_key);
        atu::hmac_sha256(mac_key, payload.data(), payload.size(), out_mac);
        // Wipe the derived keys and salt from the stack.
        ::SecureZeroMemory(mac_key, sizeof(mac_key));
        ::SecureZeroMemory(key, sizeof(key));
        ::SecureZeroMemory(salt, sizeof(salt));

        memcpy(footer_magic, "APTGEU09", 8);  // outer magic = encrypted+MAC (salted PBKDF2)
        encrypted = true;
        scr.setup["Encrypted"] = "1";
        std::printf("[pack] AES-256-CBC + PBKDF2-HMAC-SHA256(iter=%u,salt=%zuB) + HMAC 已启用 (密码长度=%zu)\n",
                    atu::kPbkdf2Iterations, atu::kSaltLen, password.size());
    }

    uint64_t payload_offset = static_cast<uint64_t>(stub.size());

    // v0.16: read DiskSliceSize
    uint64_t slice_size = 0;
    {
        auto it = scr.setup.find("DiskSliceSize");
        if (it != scr.setup.end()) {
            try { slice_size = std::stoull(it->second); } catch (...) {}
        }
    }

    // 8. Output path
    std::string out_dir = r.output_dir;
    make_dirs(out_dir);
    std::string out_path = out_dir + "/" + r.output_base_name + ".exe";

    if (encrypted && slice_size > 0) {
        throw std::runtime_error("AES 加密暂不支持 DiskSliceSize 分卷输出");
    }

    if (slice_size > 0 && payload.size() > slice_size) {
        // v0.16: slice the payload
        uint64_t total_payload = payload.size();
        uint64_t offset = 0;
        uint32_t slice_idx = 0;
        while (offset < total_payload) {
            uint64_t chunk = total_payload - offset;
            if (chunk > slice_size) chunk = slice_size;
            std::string fname = (slice_idx == 0)
                ? (out_dir + "/" + r.output_base_name + ".exe")
                : (out_dir + "/" + r.output_base_name + "-" + std::to_string(slice_idx) + ".bin");
            std::ofstream out(fname, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open: " + fname);
            if (slice_idx == 0)
                out.write(reinterpret_cast<const char*>(stub.data()), stub.size());
            out.write(reinterpret_cast<const char*>(payload.data() + offset), chunk);
            auto f = build_footer_v16(payload_offset, total_payload, slice_idx);
            out.write(reinterpret_cast<const char*>(f.data()), f.size());
            out.close();
            std::printf("[pack] 卷 %u: %s (%llu 字节)\n", slice_idx, fname.c_str(), (unsigned long long)(chunk + (slice_idx==0?stub.size():0) + f.size()));
            offset += chunk;
            slice_idx++;
        }
    } else {
        // single file output
        std::vector<uint8_t> footer;
        if (encrypted) {
            // M-02: 48-byte footer = magic[8]"APTGEU09" | payload_offset[u64 LE] | HMAC[32].
            footer.assign(48, 0);
            std::memcpy(footer.data(), "APTGEU09", 8);
            uint64_t off = payload_offset;
            for (int i = 0; i < 8; ++i) footer[8 + i] = (uint8_t)(off >> (8 * i));
            std::memcpy(footer.data() + 16, out_mac, 32);
        } else {
            footer = build_footer(payload_offset, footer_magic);
        }
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open output: " + out_path);
        out.write(reinterpret_cast<const char*>(stub.data()), stub.size());
        out.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        out.write(reinterpret_cast<const char*>(footer.data()), footer.size());
        out.close();
    }

    PackResult res;
    res.output_path = out_path;
    res.output_size = stub.size() + payload.size() + 28;
    res.file_count  = static_cast<uint32_t>(files.size());
    res.stub_size   = stub.size();
    res.ui_block_size = ui_block_size;
    res.payload_raw_size = payload_raw.size();
    res.payload_compressed_size = payload.size();
    return res;
}

// ---------------------------------------------------------------------------
// v0.2: --gen-rc
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> read_file_binary_throw(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

bool write_file_binary(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}

// "0.5.6" -> "0,5,6,0". Tolerate 1-4 components.
std::string version_to_quad(const std::string& v) {
    int a[4] = {0,0,0,0};
    int idx = 0;
    std::string cur;
    for (char c : v) {
        if (c == '.' || c == ',') {
            if (idx < 4) { try { a[idx] = std::stoi(cur); } catch (...) {} }
            idx++; cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (idx < 4 && !cur.empty()) { try { a[idx] = std::stoi(cur); } catch (...) {} }
    return std::to_string(a[0]) + "," + std::to_string(a[1]) + "," +
           std::to_string(a[2]) + "," + std::to_string(a[3]);
}

bool ieq_end(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

} // namespace

GenRcResult gen_rc(const std::string& atu_path, const std::string& out_dir) {
    AtuScript scr = parse_atu_file(atu_path);
    Resolved r = resolve_script(scr);

    make_dirs(out_dir);

    GenRcResult res;
    res.rc_path = out_dir + "\\stub.rc";

    // 1. Handle SetupIcon
    std::string setup_icon = scr.get("SetupIcon", "");
    bool has_ico = false;
    if (!setup_icon.empty()) {
        if (ieq_end(setup_icon, ".ico")) {
            if (!file_exists(setup_icon)) {
                throw std::runtime_error("SetupIcon 文件不存在: " + setup_icon);
            }
            auto bytes = read_file_binary_throw(setup_icon);
            res.icon_path = out_dir + "\\stub_icon.ico";
            if (!write_file_binary(res.icon_path, bytes)) {
                throw std::runtime_error("无法写入: " + res.icon_path);
            }
            res.icon_copied = true;
            has_ico = true;
            std::printf("[gen-rc] 图标已拷贝: %s (%zu 字节)\n",
                        res.icon_path.c_str(), bytes.size());
        } else if (ieq_end(setup_icon, ".png")) {
            std::fprintf(stderr,
                "[gen-rc] 警告: SetupIcon 暂只支持 .ico，.png 留到 v0.3 (跳过: %s)\n",
                setup_icon.c_str());
        } else {
            std::fprintf(stderr,
                "[gen-rc] 警告: SetupIcon 未知扩展名，跳过: %s\n", setup_icon.c_str());
        }
    }

    // 2. Build stub.rc
    std::string app_name    = r.app_name;
    std::string app_version  = r.app_version;
    std::string publisher    = r.publisher.empty() ? "ApTgeu" : r.publisher;
    std::string base_name    = scr.get("OutputBaseName", app_name + "-Setup");
    std::string quad         = version_to_quad(app_version);

    std::string rc;
    rc += "#include <windows.h>\n";
    if (has_ico) {
        rc += "MAINICON ICON \"stub_icon.ico\"\n";
    }
    rc += "1 VERSIONINFO\n";
    rc += "FILEVERSION     " + quad + "\n";
    rc += "PRODUCTVERSION  " + quad + "\n";
    rc += "FILEFLAGSMASK  0x3fL\n";
    rc += "FILEFLAGS      0x0L\n";
    rc += "FILEOS         0x40004L\n";
    rc += "FILETYPE       0x1L\n";
    rc += "FILESUBTYPE    0x0L\n";
    rc += "BEGIN\n";
    rc += "  BLOCK \"StringFileInfo\"\n";
    rc += "  BEGIN\n";
    rc += "    BLOCK \"040904b0\"\n";
    rc += "    BEGIN\n";
    auto q = [&](const std::string& k, const std::string& v) {
        rc += "      VALUE \"" + k + "\",      \"" + v + "\"\n";
    };
    q("CompanyName",       publisher);
    q("FileDescription",   app_name + " Setup");
    q("FileVersion",       app_version);
    q("InternalName",      base_name);
    q("LegalCopyright",    "Copyright (C) 2026 " + publisher);
    q("OriginalFilename",   base_name + ".exe");
    q("ProductName",       app_name);
    q("ProductVersion",    app_version);
    rc += "    END\n";
    rc += "  END\n";
    rc += "  BLOCK \"VarFileInfo\"\n";
    rc += "  BEGIN\n";
    rc += "    VALUE \"Translation\", 0x409, 1200\n";
    rc += "  END\n";
    rc += "END\n";

    // v0.13 B5: manifest (requireAdministrator vs asInvoker).
    std::string priv = "asInvoker";
    std::string dd = r.default_dir;
    // If default dir contains "Program Files" or "pf" -> admin.
    if (dd.find("Program Files") != std::string::npos ||
        dd.find("\\pf\\") != std::string::npos ||
        dd.find("\\pf") == 0) {
        priv = "requireAdministrator";
    }
    std::string manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
        "    <security>\n"
        "      <requestedPrivileges>\n"
        "        <requestedExecutionLevel level=\"" + priv + "\" uiAccess=\"false\"/>\n"
        "      </requestedPrivileges>\n"
        "    </security>\n"
        "  </trustInfo>\n"
        "</assembly>\n";
    // Write manifest file and reference it from RC.
    std::string manifest_path = out_dir + "\\stub.manifest";
    write_file_binary(manifest_path, std::vector<uint8_t>(manifest.begin(), manifest.end()));
    rc += "\n// Manifest (B5)\n";
    rc += "1 24 \"" + std::string(manifest_path.begin() + out_dir.size() + 1, manifest_path.end()) + "\"\n";
    std::printf("[gen-rc] manifest privilege: %s\n", priv.c_str());

    if (!write_file_binary(res.rc_path,
            std::vector<uint8_t>(rc.begin(), rc.end()))) {
        throw std::runtime_error("无法写入: " + res.rc_path);
    }
    std::printf("[gen-rc] 已生成: %s\n", res.rc_path.c_str());
    return res;
}

} // namespace atu
