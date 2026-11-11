/*
 * ApTgeu 32-bit GUI installer stub (Dear ImGui + Win32 + OpenGL3).
 *
 * 8-page wizard:
 *   Welcome -> License -> Readme -> Dir -> Components -> Ready
 *           -> Preparing -> Progress -> Finish
 *
 * Self-contained: no dependency on the 64-bit stub's payload/i18n/theme tree.
 * Reuses the *per-file LZMA2 decompression* loop of src/stub32/main.cpp.
 *
 * Build: i686-w64-mingw32 g++ (see build_stub32_gui.ps1).
 *
 * Layout read from own exe footer (28 bytes):
 *   [0..7]   magic "APTGEU06" / "APTGEU03"
 *   [8..15]  payload_offset  LE uint64
 *   [16..23] total_payload   LE uint64
 * Payload:
 *   u32 config_len   + bytes
 *   u32 ui_len       + bytes
 *   u32 license_len  + bytes   (NEW: license.txt content, UTF-8)
 *   u32 readme_len   + bytes   (NEW: readme.txt content, UTF-8)
 *   u32 file_count
 *   per file:
 *     u32 rel_len + utf8 rel
 *     u32 comp_len  + bytes (skipped)
 *     u32 tasks_len + bytes (skipped)
 *     u32 flags_len + bytes (skipped)
 *     u32 raw_size, u32 crc, u8 comp
 *     raw_size bytes of file data
 *         comp==1: byte prop | u64 uncomp_size | lzma2 stream
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

extern "C" {
#include "Lzma2Dec.h"
}

// ---------------------------------------------------------------------------
// LZMA2 allocator
// ---------------------------------------------------------------------------
static void* LzmaAlloc(ISzAllocPtr, size_t sz) { return malloc(sz); }
static void  LzmaFree(ISzAllocPtr, void* p)     { free(p); }
static ISzAlloc g_lzma = { &LzmaAlloc, &LzmaFree };

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t rd_u64(const uint8_t* p) {
    uint64_t lo = rd_u32(p);
    uint64_t hi = rd_u32(p+4);
    return lo | (hi << 32);
}
static std::wstring utf8_to_wide(const char* s, int len = -1) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// Payload / install state
// ---------------------------------------------------------------------------
enum class WizardPage {
    Welcome, License, Readme, Dir, Components, Ready, Preparing, Progress, Finish
};

struct FileEntry {
    std::string rel;     // utf-8 relative path
    uint8_t  comp = 0;
    uint32_t raw_size = 0;
    uint32_t data_off = 0;  // offset within payload
    int      component = 0; // index into g_components
};

struct Component {
    std::string name;      // display name
    std::string prefix;    // path prefix that maps to this component
    bool        required = false;
    bool        checked = true;
};

static std::vector<FileEntry> g_files;
static std::vector<Component> g_components;
static std::string g_license_text;
static std::string g_readme_text;
static const uint8_t* g_payload = nullptr;   // malloc'd
static size_t         g_payload_len = 0;

struct WizardState {
    WizardPage page = WizardPage::Welcome;
    char  install_dir_buf[4096] = {0};
    bool  request_exit = false;

    // License page: 0=unset, 1=accept, 2=decline
    int   license_choice = 0;

    // install progress
    int   install_index = 0;
    float progress = 0.0f;
    std::string current_file;
    bool  install_done = false;
    bool  install_failed = false;
    std::string error_msg;
};

// Autopilot (driven by an external named event for headless E2E).
static bool g_autopilot = false;

// ---------------------------------------------------------------------------
// Component derivation: group files by top-level directory.
// Files directly in root -> "Core" (required).
// Files under subdir/ -> one component per subdir.
// ---------------------------------------------------------------------------
static void derive_components() {
    g_components.clear();
    // Component 0: Core (required) - files with no '/' in rel
    Component core;
    core.name = "核心程序 (Core)";
    core.prefix = "";
    core.required = true;
    core.checked = true;
    g_components.push_back(core);

    for (auto& f : g_files) {
        size_t slash = f.rel.find('/');
        int cidx = 0;
        if (slash != std::string::npos) {
            std::string top = f.rel.substr(0, slash);
            // find or create
            cidx = -1;
            for (size_t i = 1; i < g_components.size(); i++) {
                if (g_components[i].prefix == top) { cidx = (int)i; break; }
            }
            if (cidx < 0) {
                Component c;
                c.name = "组件: " + top;
                c.prefix = top;
                c.required = false;
                c.checked = true;
                g_components.push_back(c);
                cidx = (int)g_components.size() - 1;
            }
        }
        f.component = cidx;
    }
}

// ---------------------------------------------------------------------------
// Payload parsing
// ---------------------------------------------------------------------------
static bool read_payload(std::string& err) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "open self failed"; return false; }
    DWORD fsize = GetFileSize(h, nullptr);

    SetFilePointer(h, (LONG)(fsize - 28), nullptr, FILE_BEGIN);
    uint8_t footer[28]; DWORD rd = 0;
    ReadFile(h, footer, 28, &rd, nullptr);

    if (memcmp(footer, "APTGEU06", 8) != 0 && memcmp(footer, "APTGEU03", 8) != 0) {
        CloseHandle(h);
        err = "bad footer magic (no payload appended)";
        return false;
    }
    uint64_t payload_offset = rd_u64(footer + 8);
    uint64_t total_payload  = rd_u64(footer + 16);
    if (total_payload == 0 || payload_offset >= fsize) {
        CloseHandle(h); err = "bad footer offsets"; return false;
    }

    uint8_t* buf = (uint8_t*)malloc((size_t)total_payload);
    if (!buf) { CloseHandle(h); err = "oom"; return false; }
    SetFilePointer(h, (LONG)payload_offset, nullptr, FILE_BEGIN);
    ReadFile(h, buf, (DWORD)total_payload, &rd, nullptr);
    CloseHandle(h);

    g_payload = buf;
    g_payload_len = (size_t)total_payload;

    uint32_t pos = 0;
    uint32_t plen = (uint32_t)total_payload;
    if (pos + 4 > plen) { err = "config len"; return false; }
    uint32_t config_len = rd_u32(buf + pos); pos += 4 + config_len;
    if (pos + 4 > plen) { err = "ui len"; return false; }
    uint32_t ui_len = rd_u32(buf + pos); pos += 4 + ui_len;
    if (pos + 4 > plen) { err = "license len"; return false; }
    uint32_t license_len = rd_u32(buf + pos); pos += 4;
    if (pos + license_len > plen) { err = "license overflow"; return false; }
    g_license_text.assign((const char*)(buf + pos), license_len); pos += license_len;
    if (pos + 4 > plen) { err = "readme len"; return false; }
    uint32_t readme_len = rd_u32(buf + pos); pos += 4;
    if (pos + readme_len > plen) { err = "readme overflow"; return false; }
    g_readme_text.assign((const char*)(buf + pos), readme_len); pos += readme_len;
    if (pos + 4 > plen) { err = "file count"; return false; }
    uint32_t file_count = rd_u32(buf + pos); pos += 4;

    for (uint32_t i = 0; i < file_count; i++) {
        if (pos + 4 > plen) { err = "rp_len"; return false; }
        uint32_t rp_len = rd_u32(buf + pos); pos += 4;
        if (pos + rp_len > plen) { err = "rel"; return false; }
        FileEntry e;
        e.rel.assign((const char*)(buf + pos), rp_len);
        pos += rp_len;

        if (pos + 4 > plen) { err = "comp_len"; return false; }
        uint32_t comp_len = rd_u32(buf + pos); pos += 4 + comp_len;
        if (pos + 4 > plen) { err = "tasks_len"; return false; }
        uint32_t tasks_len = rd_u32(buf + pos); pos += 4 + tasks_len;
        if (pos + 4 > plen) { err = "flags_len"; return false; }
        uint32_t flags_len = rd_u32(buf + pos); pos += 4 + flags_len;
        if (pos + 9 > plen) { err = "hdr"; return false; }
        e.raw_size = rd_u32(buf + pos); pos += 4;
        pos += 4; // crc
        e.comp = buf[pos]; pos += 1;
        if (pos + e.raw_size > plen) { err = "data overflow"; return false; }
        e.data_off = pos;
        g_files.push_back(e);
        pos += e.raw_size;
    }

    derive_components();
    return true;
}

// ---------------------------------------------------------------------------
// File writing helpers (ported from src/stub32/main.cpp)
// ---------------------------------------------------------------------------
static void create_parent_dirs(const wchar_t* full) {
    wchar_t tmp[MAX_PATH];
    int n = lstrlenW(full);
    int slash = -1;
    for (int i = 0; i < n; i++) if (full[i] == L'\\') slash = i;
    if (slash < 2) return;
    lstrcpynW(tmp, full, MAX_PATH);
    tmp[slash] = 0;
    for (int i = 3; i < (int)lstrlenW(tmp); i++) {
        if (tmp[i] == L'\\') {
            wchar_t save = tmp[i]; tmp[i] = 0;
            CreateDirectoryW(tmp, nullptr);
            tmp[i] = save;
        }
    }
    CreateDirectoryW(tmp, nullptr);
}

// Install exactly one file by index. Returns true on success.
static bool install_one_file(const WizardState* st, int idx, std::string& err_out) {
    const FileEntry& e = g_files[idx];

    // Build wide full path with fixed buffers (same approach as stub32/main.cpp).
    wchar_t wrel[MAX_PATH] = {0};
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, e.rel.c_str(), (int)e.rel.size(),
                                    wrel, MAX_PATH - 1);
        for (int i = 0; i < n; i++) if (wrel[i] == L'/') wrel[i] = L'\\';
    }
    wchar_t wdir[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, st->install_dir_buf, -1, wdir, MAX_PATH - 1);

    wchar_t full[MAX_PATH] = {0};
    lstrcpynW(full, wdir, MAX_PATH);
    int dl = lstrlenW(full);
    if (dl > 0 && full[dl-1] != L'\\') { full[dl] = L'\\'; full[dl+1] = 0; }
    lstrcatW(full, wrel);

    create_parent_dirs(full);

    HANDLE hf = CreateFileW(full, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        char u8[MAX_PATH] = {0};
        WideCharToMultiByte(CP_UTF8, 0, full, -1, u8, MAX_PATH, nullptr, nullptr);
        err_out = std::string("CreateFileW failed: ") + u8;
        return false;
    }

    bool ok = false;
    const uint8_t* data = g_payload + e.data_off;
    if (e.comp == 0) {
        DWORD w = 0;
        WriteFile(hf, data, e.raw_size, &w, nullptr);
        ok = (w == e.raw_size);
    } else {
        if (e.raw_size >= 9) {
            Byte prop = data[0];
            uint64_t uncomp_size = rd_u64(data + 1);
            const Byte* stream = data + 9;
            SizeT stream_len = (SizeT)(e.raw_size - 9);
            uint8_t* outbuf = (uint8_t*)malloc((size_t)uncomp_size);
            if (outbuf) {
                SizeT dest_len = (SizeT)uncomp_size;
                SizeT src_len  = stream_len;
                ELzmaStatus status;
                SRes rc = Lzma2Decode(outbuf, &dest_len, stream, &src_len,
                                      prop, LZMA_FINISH_END, &status, &g_lzma);
                if (rc == SZ_OK && status == LZMA_STATUS_FINISHED_WITH_MARK) {
                    DWORD w = 0;
                    WriteFile(hf, outbuf, (DWORD)dest_len, &w, nullptr);
                    ok = (w == (DWORD)dest_len);
                } else {
                    err_out = "LZMA2 decode rc=" + std::to_string(rc) +
                              " status=" + std::to_string((int)status);
                }
                free(outbuf);
            } else {
                err_out = "oom for decompress";
            }
        } else {
            err_out = "compressed file too small";
        }
    }
    CloseHandle(hf);
    if (!ok && err_out.empty()) err_out = "write failed";
    return ok;
}

// Count files that will actually be installed (component checked).
static int count_available_files() {
    int n = 0;
    for (size_t i = 0; i < g_files.size(); i++) {
        if (g_components[g_files[i].component].checked) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Wizard page rendering
// ---------------------------------------------------------------------------
static void draw_footer_buttons(WizardState* st, bool back_enabled,
                                bool next_enabled, const char* next_label,
                                bool advance) {
    float button_w = 96.0f;
    float spacing  = 8.0f;
    float total_w = button_w * 3 + spacing * 2;
    float start_x = (ImGui::GetContentRegionMax().x - total_w) * 0.5f;
    ImGui::SetCursorPosX(start_x);

    if (ImGui::Button("取消", ImVec2(button_w, 0))) st->request_exit = true;
    ImGui::SameLine(0, spacing);
    if (back_enabled && ImGui::Button("< 上一步", ImVec2(button_w, 0))) {
        switch (st->page) {
        case WizardPage::License:    st->page = WizardPage::Welcome; break;
        case WizardPage::Readme:     st->page = WizardPage::License; break;
        case WizardPage::Dir:        st->page = WizardPage::Readme; break;
        case WizardPage::Components: st->page = WizardPage::Dir; break;
        case WizardPage::Ready:      st->page = WizardPage::Components; break;
        default: break;
        }
    }
    ImGui::SameLine(0, spacing);
    if (next_enabled && (ImGui::Button(next_label, ImVec2(button_w, 0)) || advance)) {
        switch (st->page) {
        case WizardPage::Welcome:    st->page = WizardPage::License; break;
        case WizardPage::License:    st->page = WizardPage::Readme; break;
        case WizardPage::Readme:     st->page = WizardPage::Dir; break;
        case WizardPage::Dir:        st->page = WizardPage::Components; break;
        case WizardPage::Components: st->page = WizardPage::Ready; break;
        case WizardPage::Ready:      st->page = WizardPage::Preparing; break;
        case WizardPage::Preparing:  st->page = WizardPage::Progress; break;
        case WizardPage::Progress:   st->page = WizardPage::Finish; break;
        case WizardPage::Finish:     st->request_exit = true; break;
        }
    }
}

static void page_welcome(WizardState* st, bool advance) {
    ImGui::NewLine(); ImGui::NewLine();
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextWrapped("%s", "ApTgeu 安装向导");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::NewLine();
    ImGui::TextWrapped("这是 ApTgeu 32 位 GUI 安装程序（Dear ImGui）。\n\n"
                       "点击“下一步”继续，或“取消”退出安装。\n"
                       "本向导共 8 页：欢迎 -> 许可 -> 说明 -> 目录 -> "
                       "组件 -> 准备 -> 进度 -> 完成。");
    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    draw_footer_buttons(st, false, true, "下一步 >", advance);
}

static void page_license(WizardState* st, bool advance) {
    ImGui::SeparatorText("许可协议");
    ImGui::NewLine();
    ImGui::TextWrapped("请仔细阅读下面的许可协议。要继续安装，您必须接受该协议。");
    ImGui::NewLine();

    // Scrollable license text
    ImGui::BeginChild("##license", ImVec2(0, 260), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (!g_license_text.empty()) {
        ImGui::TextUnformatted(g_license_text.c_str());
    } else {
        ImGui::TextWrapped("（未在载荷中找到 license.txt 内容）");
    }
    ImGui::EndChild();

    ImGui::NewLine();
    ImGui::Text("请选择:");
    ImGui::RadioButton("我接受该许可协议", &st->license_choice, 1);
    ImGui::RadioButton("我不接受该协议",   &st->license_choice, 2);

    // Autopilot: auto-accept so the wizard can proceed.
    if (g_autopilot && st->license_choice == 0) st->license_choice = 1;

    bool ok = (st->license_choice == 1);
    if (!ok && st->license_choice == 2) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                           "您必须接受许可协议才能继续安装。");
    }
    draw_footer_buttons(st, true, ok, "下一步 >", advance && ok);
}

static void page_readme(WizardState* st, bool advance) {
    ImGui::SeparatorText("安装说明 (Readme)");
    ImGui::NewLine();

    ImGui::BeginChild("##readme", ImVec2(0, 320), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (!g_readme_text.empty()) {
        ImGui::TextUnformatted(g_readme_text.c_str());
    } else {
        ImGui::TextWrapped("（未在载荷中找到 readme.txt 内容）");
    }
    ImGui::EndChild();

    ImGui::NewLine();
    draw_footer_buttons(st, true, true, "下一步 >", advance);
}

static void page_dir(WizardState* st, bool advance) {
    ImGui::SeparatorText("选择安装目录");
    ImGui::NewLine();
    ImGui::TextWrapped("安装程序将把文件解压到下面的目录。");
    ImGui::NewLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("目录:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-130.0f);
    ImGui::InputText("##dir", st->install_dir_buf, sizeof(st->install_dir_buf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("浏览...", ImVec2(120, 0))) {
        BROWSEINFOW bi; ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = nullptr;
        bi.lpszTitle = L"选择安装文件夹";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) {
                WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                    st->install_dir_buf, sizeof(st->install_dir_buf),
                                    nullptr, nullptr);
            }
            CoTaskMemFree(pidl);
        }
    }

    ImGui::NewLine();
    bool disk_ok = true;
    {
        std::wstring wdir = utf8_to_wide(st->install_dir_buf);
        ULARGE_INTEGER freeb;
        if (!wdir.empty() && GetDiskFreeSpaceExW(wdir.c_str(), nullptr, nullptr, &freeb)) {
            double free_mb = (double)freeb.QuadPart / (1024.0 * 1024.0);
            ImGui::Text("该盘可用空间: %.1f MB", free_mb);
            if (free_mb < 1.0) {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "磁盘空间不足!");
                disk_ok = false;
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                               "（目录尚不存在，安装时会自动创建）");
        }
    }

    bool ok = st->install_dir_buf[0] != '\0' && disk_ok;
    draw_footer_buttons(st, true, ok, "下一步 >", advance && ok);
}

static void page_components(WizardState* st, bool advance) {
    ImGui::SeparatorText("选择组件");
    ImGui::NewLine();
    ImGui::TextWrapped("勾选您希望安装的组件。核心程序为必选项。");
    ImGui::NewLine();

    for (size_t i = 0; i < g_components.size(); i++) {
        Component& c = g_components[i];
        int file_count = 0;
        for (auto& f : g_files) if (f.component == (int)i) file_count++;
        ImGui::BeginGroup();
        if (c.required) {
            // Required: show a checked, disabled checkbox
            bool chk = true;
            ImGui::Checkbox("##req", &chk);
            ImGui::BeginDisabled(); ImGui::SameLine();
            ImGui::Text("%s  [%d 个文件]  (必选)", c.name.c_str(), file_count);
            ImGui::EndDisabled();
        } else {
            ImGui::Checkbox(c.name.c_str(), &c.checked);
            ImGui::SameLine();
            ImGui::TextDisabled("[%d 个文件]", file_count);
        }
        ImGui::EndGroup();
    }

    ImGui::NewLine();
    int avail = count_available_files();
    ImGui::Text("将安装 %d / %d 个文件。", avail, (int)g_files.size());
    draw_footer_buttons(st, true, avail > 0, "下一步 >", advance && avail > 0);
}

static void page_ready(WizardState* st, bool advance) {
    ImGui::SeparatorText("准备安装");
    ImGui::NewLine();
    ImGui::TextWrapped("安装程序已准备好把下列内容安装到您的计算机。");
    ImGui::NewLine();

    ImGui::Text("目标目录:");
    ImGui::Indent(20.0f);
    ImGui::TextUnformatted(st->install_dir_buf);
    ImGui::Unindent(20.0f);
    ImGui::NewLine();

    ImGui::Text("将安装的组件:");
    ImGui::Indent(20.0f);
    for (size_t i = 0; i < g_components.size(); i++) {
        const Component& c = g_components[i];
        if (!c.checked && !c.required) continue;
        ImGui::BulletText("%s", c.name.c_str());
    }
    ImGui::Unindent(20.0f);
    ImGui::NewLine();

    int avail = count_available_files();
    ImGui::Text("共 %d 个文件将被解压。", avail);
    {
        std::wstring wdir = utf8_to_wide(st->install_dir_buf);
        ULARGE_INTEGER freeb;
        if (!wdir.empty() && GetDiskFreeSpaceExW(wdir.c_str(), nullptr, nullptr, &freeb)) {
            double free_mb = (double)freeb.QuadPart / (1024.0 * 1024.0);
            ImGui::Text("目标盘可用空间: %.1f MB", free_mb);
        }
    }

    ImGui::NewLine();
    draw_footer_buttons(st, true, true, "安装 >", advance);
}

static void page_preparing(WizardState* st, bool advance) {
    ImGui::NewLine(); ImGui::NewLine();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextWrapped("正在准备安装...");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::NewLine();
    ImGui::TextWrapped("安装程序正在初始化解压环境，请稍候。");
    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    draw_footer_buttons(st, false, true, "开始安装 >", advance);
}

static void page_progress(WizardState* st, bool advance) {
    ImGui::SeparatorText("正在安装");
    ImGui::NewLine();
    ImGui::Text("正在解压文件，请稍候...");
    ImGui::NewLine();

    // Install at most one file per frame (mirrors the 64-bit stub).
    if (!st->install_done && !st->install_failed) {
        // Walk forward, skipping files whose component is unchecked.
        while (st->install_index < (int)g_files.size()) {
            const FileEntry& e = g_files[st->install_index];
            if (!g_components[e.component].checked) {
                st->install_index++;
                continue;
            }
            st->current_file = e.rel;
            std::string err;
            bool ok = install_one_file(st, st->install_index, err);
            if (!ok) {
                st->install_failed = true;
                st->error_msg = err;
            } else {
                st->install_index++;
                int total = count_available_files();
                int done = 0;
                for (int i = 0; i < st->install_index; i++) {
                    if (g_components[g_files[i].component].checked) done++;
                }
                st->progress = (total <= 0) ? 1.0f : (float)done / (float)total;
            }
            break; // one file per frame
        }
        if (st->install_index >= (int)g_files.size()) {
            st->install_done = true;
            st->progress = 1.0f;
        }
    }

    ImGui::ProgressBar(st->progress, ImVec2(-FLT_MIN, 0));
    ImGui::NewLine();
    if (!st->current_file.empty())
        ImGui::Text("当前文件: %s", st->current_file.c_str());

    if (st->install_failed) {
        ImGui::NewLine();
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "安装失败: %s",
                           st->error_msg.c_str());
        draw_footer_buttons(st, false, true, "关闭", advance);
        return;
    }

    // Stay on progress until install done; then allow advance.
    draw_footer_buttons(st, false, st->install_done,
                        st->install_done ? "下一步 >" : "请稍候...",
                        advance && st->install_done);
}

static void page_finish(WizardState* st, bool advance) {
    ImGui::NewLine(); ImGui::NewLine();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextWrapped("安装完成");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::NewLine();
    ImGui::TextWrapped("ApTgeu 已成功解压到:\n  %s", st->install_dir_buf);
    ImGui::NewLine();
    int total_written = 0;
    for (int i = 0; i < st->install_index; i++) {
        if (g_components[g_files[i].component].checked) total_written++;
    }
    ImGui::Text("共写入 %d 个文件。", total_written);
    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    draw_footer_buttons(st, false, true, "完成", advance);
}

static void wizard_render(WizardState* st, bool advance) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##wizard", nullptr, flags);

    switch (st->page) {
    case WizardPage::Welcome:    page_welcome(st, advance);    break;
    case WizardPage::License:    page_license(st, advance);    break;
    case WizardPage::Readme:     page_readme(st, advance);     break;
    case WizardPage::Dir:        page_dir(st, advance);        break;
    case WizardPage::Components: page_components(st, advance); break;
    case WizardPage::Ready:      page_ready(st, advance);      break;
    case WizardPage::Preparing:  page_preparing(st, advance);  break;
    case WizardPage::Progress:   page_progress(st, advance);   break;
    case WizardPage::Finish:     page_finish(st, advance);     break;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// WGL boilerplate (from imgui example_win32_opengl3)
// ---------------------------------------------------------------------------
struct WGL_WindowData { HDC hDC; };
static HGLRC          g_hRC;
static WGL_WindowData g_MainWindow;
static int            g_Width  = 800;
static int            g_Height = 500;

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    HDC hDc = GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(hDc, &pfd);
    if (pf == 0) return false;
    if (SetPixelFormat(hDc, pf, &pfd) == FALSE) return false;
    ReleaseDC(hWnd, hDc);
    data->hDC = GetDC(hWnd);
    if (g_hRC) return true;
    HGLRC tempRC = wglCreateContext(data->hDC);
    if (!tempRC) { ReleaseDC(hWnd, data->hDC); return false; }
    if (!wglMakeCurrent(data->hDC, tempRC)) {
        wglDeleteContext(tempRC); ReleaseDC(hWnd, data->hDC); return false;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(0x821B, &major);
    glGetIntegerv(0x821C, &minor);
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (major == 0 && minor == 0) sscanf(ver, "%d.%d", &major, &minor);
    GLuint gl_ver = (GLuint)(major * 100 + minor * 10);
    g_hRC = tempRC;
    if (gl_ver >= 300) return true;
    typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    auto wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    const int attribs[] = { 0x2091, 3, 0x2092, 0, 0x9126, 0x0001, 0 };
    HGLRC newRC = nullptr;
    if (wglCreateContextAttribsARB)
        newRC = wglCreateContextAttribsARB(data->hDC, 0, attribs);
    if (newRC) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        g_hRC = newRC;
        wglMakeCurrent(data->hDC, g_hRC);
    }
    return true;
}
static void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    wglMakeCurrent(nullptr, nullptr);
    ReleaseDC(hWnd, data->hDC);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_Width = LOWORD(lParam); g_Height = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Stashed /DIR= value (defined at file bottom).
static std::wstring g_cli_dir;

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // ---- command line: /DIR=... /AUTOPILOT ----
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; i++) {
                std::wstring a = argv[i];
                if (_wcsicmp(a.c_str(), L"/AUTOPILOT") == 0) g_autopilot = true;
                else if (wcsnicmp(a.c_str(), L"/DIR=", 5) == 0) {
                    std::wstring v = a.substr(5);
                    if (!v.empty() && v.front()==L'"' && v.back()==L'"')
                        v = v.substr(1, v.size()-2);
                    g_cli_dir = v;
                }
            }
            LocalFree(argv);
        }
    }

    // ---- payload ----
    WizardState state;
    std::string perr;
    bool have_payload = read_payload(perr);
    if (!have_payload) {
        // No payload: still pop a MessageBox so "能弹窗" is demonstrable, then exit.
        std::wstring w = L"ApTgeu 32-bit stub\n\n未找到附加载荷 (footer): ";
        w += utf8_to_wide(perr.c_str());
        w += L"\n\n这是一个无载荷的 stub；附加 APTGEU06 载荷后会显示安装向导。";
        MessageBoxW(nullptr, w.c_str(), L"ApTgeu Setup 32-bit",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // default install dir
    if (!g_cli_dir.empty()) {
        std::string u8(g_cli_dir.begin(), g_cli_dir.end());
        strncpy(state.install_dir_buf, u8.c_str(), sizeof(state.install_dir_buf)-1);
    } else {
        strncpy(state.install_dir_buf, "C:\\ApTgeuApp32", sizeof(state.install_dir_buf)-1);
    }

    // ---- window + GL ----
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, WndProc, 0L, 0L,
                       hInst, nullptr, LoadCursor(nullptr, IDC_ARROW),
                       (HBRUSH)(COLOR_WINDOW+1), nullptr,
                       L"ApTgeuStub32Gui", nullptr };
    RegisterClassExW(&wc);

    int win_w = (int)(800 * scale);
    int win_h = (int)(560 * scale);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ApTgeu 安装程序 (32-bit)",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              sx, sy, win_w, win_h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { MessageBoxW(nullptr, L"CreateWindow failed", L"err", MB_OK); return 1; }
    if (!CreateDeviceWGL(hwnd, &g_MainWindow)) {
        MessageBoxW(nullptr, L"OpenGL context failed", L"err", MB_OK); return 1;
    }
    wglMakeCurrent(g_MainWindow.hDC, g_hRC);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f);
    if (font) io.FontDefault = font;

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    HANDLE stepEvent = nullptr;
    if (g_autopilot) {
        // Open (or create) the event the E2E script uses to advance pages.
        stepEvent = CreateEventW(nullptr, TRUE, FALSE, L"ApTgeuStepEvent");
    }

    bool done = false;
    bool release = false;   // autopilot: advance the wizard on next render
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) { Sleep(10); continue; }

        // Autopilot gate: block here (rendering the current page) until the
        // external event is signaled, then release one advance.
        if (g_autopilot && stepEvent && !release) {
            for (;;) {
                MSG m2;
                while (PeekMessage(&m2, nullptr, 0U, 0U, PM_REMOVE)) {
                    TranslateMessage(&m2); DispatchMessage(&m2);
                    if (m2.message == WM_QUIT) { done = true; break; }
                }
                if (done) break;
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                wizard_render(&state, false);
                ImGui::Render();
                glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
                glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                SwapBuffers(g_MainWindow.hDC);

                DWORD wr = WaitForSingleObject(stepEvent, 50);
                if (wr == WAIT_OBJECT_0) { ResetEvent(stepEvent); release = true; break; }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        wizard_render(&state, release);
        release = false;
        if (state.request_exit) PostQuitMessage(0);
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_MainWindow.hDC);
    }

    if (stepEvent) CloseHandle(stepEvent);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceWGL(hwnd, &g_MainWindow);
    wglDeleteContext(g_hRC);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
