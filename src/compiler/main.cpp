/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu compiler entry point
//   aptgeu.exe <script.atu> [--ui <path.atui>]              (pack)
//   aptgeu.exe --gen-rc <script.atu> [--out-dir build]      (emit stub.rc + stub_icon.ico)
#include "packer.h"

// v0.16: minimal PE32 stub generator
bool write_32bit_stub(const std::string& path);

// v0.17: .iss compat
namespace atu { struct AtuScript; AtuScript parse_iss_file(const std::string& path); }

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string atu_path;
    std::string ui_override;
    std::string out_dir = "build";
    bool gen_rc_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gen-rc") {
            gen_rc_mode = true;
        } else if (a == "--ui" || a == "/ui") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "用法: aptgeu.exe <script.atu> [--ui <path.atui>]\n");
                return 2;
            }
            ui_override = argv[++i];
        } else if (a == "--out-dir" || a == "/out-dir") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "用法: aptgeu.exe --gen-rc <script.atu> [--out-dir <dir>]\n");
                return 2;
            }
            out_dir = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            atu_path = a;
        }
    }

    if (gen_rc_mode) {
        if (atu_path.empty()) {
            std::fprintf(stderr, "用法: aptgeu.exe --gen-rc <script.atu> [--out-dir <dir>]\n");
            return 2;
        }
        try {
            auto res = atu::gen_rc(atu_path, out_dir);
            std::printf("RC  输出: %s\n", res.rc_path.c_str());
            if (res.icon_copied) {
                std::printf("ICO 输出: %s\n", res.icon_path.c_str());
            } else {
                std::printf("ICO 输出: (无图标，RC 中不含 MAINICON 行)\n");
            }
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[aptgeu --gen-rc] 错误: %s\n", e.what());
            return 1;
        }
    }

    if (atu_path.empty()) {
        std::fprintf(stderr, "用法: aptgeu.exe <script.atu> [--ui <path.atui>]\n");
        return 2;
    }
    std::string stub_path = atu::own_exe_dir() + "\\stub.exe";

    // v0.17: .iss support - convert to temp .atu
    std::string temp_atu;
    if (atu_path.size() > 4 && atu_path.substr(atu_path.size()-4) == ".iss") {
        std::printf("[.iss] 检测到 Inno Setup 脚本，转换中...\n");
        atu::AtuScript scr = atu::parse_iss_file(atu_path);
        // write temp .atu
        temp_atu = atu::own_exe_dir() + "\\_temp.atu";
        std::ofstream of(temp_atu);
        of << "[Setup]\n";
        for (auto& [k,v] : scr.setup) of << k << "=" << v << "\n";
        of << "\n[Files]\n";
        for (auto& f : scr.files) of << "Source: \"" << f.source << "\"; DestDir: \"" << f.destdir << "\"\n";
        of.close();
        atu_path = temp_atu;
    }

    try {
        auto res = atu::pack(atu_path, stub_path, ui_override);
        std::printf("输出路径 : %s\n", res.output_path.c_str());
        std::printf("输出大小 : %llu 字节 (%.2f KB)\n",
                    (unsigned long long)res.output_size,
                    (double)res.output_size / 1024.0);
        std::printf("文件数   : %u\n", res.file_count);
        std::printf("stub 大小: %llu 字节\n", (unsigned long long)res.stub_size);
        std::printf("UI 块大小: %u 字节\n", res.ui_block_size);
        if (res.payload_raw_size > 0) {
            double ratio = (double)res.payload_compressed_size / (double)res.payload_raw_size * 100.0;
            std::printf("payload 原始: %llu 字节\n", (unsigned long long)res.payload_raw_size);
            std::printf("payload 压缩: %llu 字节 (%.1f%%)\n",
                        (unsigned long long)res.payload_compressed_size, ratio);
        }
        // v0.16: write 32-bit stub (friendly messagebox for 32-bit Windows)
        {
            std::string out32 = atu::own_exe_dir() + "\\..\\Output\\setup32.exe";
            // resolve: own_exe_dir is build/, Output is sibling
            out32 = atu::own_exe_dir() + "\\setup32.exe";
            if (write_32bit_stub(out32)) {
                std::printf("32位 stub: %s\n", out32.c_str());
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
