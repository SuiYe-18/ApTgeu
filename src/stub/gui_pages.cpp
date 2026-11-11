/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "gui_pages.h"
#include "registry.h"
#include "ini.h"
#include "i18n.h"
#include "log.h"

#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <objbase.h>
#include <shellapi.h>
#include <GL/gl.h>

// stb_image: implement in this TU.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cstdio>
#include <cctype>
#include <string>

namespace stub {

// ---------------------------------------------------------------------------
// v0.2.5: required-disk-space estimate.
//   sum(raw_size of every on-disk file) + standalone uninstaller size,
//   then 10% headroom. Files whose rel_path starts with "APTGEU::" are
//   theme/metadata entries and never hit the install dir, so they are
//   excluded.
// ---------------------------------------------------------------------------
static double compute_required_mb(const ParsedPayload& p) {
    uint64_t total = 0;
    for (const auto& f : p.files) {
        if (f.rel_path.rfind("APTGEU::", 0) == 0) continue;
        total += f.raw_size;
    }
    // Standalone uninstaller is also written into the install dir.
    if (!p.unins_exe_data.empty()) {
        total += p.unins_exe_data.size();
    } else {
        // Fallback: we copy the running stub itself to unins000.exe.
        wchar_t self_path[MAX_PATH] = {0};
        DWORD n = ::GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            HANDLE h = ::CreateFileW(self_path, GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz = {0};
                if (::GetFileSizeEx(h, &sz)) total += (uint64_t)sz.QuadPart;
                ::CloseHandle(h);
            }
        }
    }
    // +10% margin for metadata, shortcuts, NTFS overhead.
    double mb = (double)total * 1.10 / (1024.0 * 1024.0);
    if (mb < 1.0) mb = 1.0;  // floor at 1 MB so the check is always meaningful
    return mb;
}

// v0.2.5: GetDiskFreeSpaceExW requires the directory to exist. If the
// install dir doesn't exist yet (normal on the Dir page / at silent-install
// start), fall back to the drive root "<X>:\".
static bool get_free_bytes(const std::wstring& path, ULARGE_INTEGER* out_free) {
    if (::GetDiskFreeSpaceExW(path.c_str(), nullptr, nullptr, out_free)) return true;
    if (path.size() >= 2 && path[1] == L':') {
        std::wstring root = path.substr(0, 2) + L"\\";
        if (::GetDiskFreeSpaceExW(root.c_str(), nullptr, nullptr, out_free)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Theme -> GL textures
// ---------------------------------------------------------------------------

static GLuint load_png_texture(const std::vector<uint8_t>& png, int& out_w, int& out_h) {
    out_w = 0; out_h = 0;
    if (png.empty()) return 0;
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(
        (const stbi_uc*)png.data(), (int)png.size(),
        &w, &h, &comp, 4);
    if (!px) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    out_w = w; out_h = h;
    return tex;
}

void wizard_init_theme_textures(WizardState* s) {
    if (!s || !s->payload) return;
    const UiTheme& t = s->payload->theme;

    // UI block takes priority over legacy APTGEU::welcome.png.
    if (!t.welcome_banner_png.empty()) {
        s->welcome_tex = load_png_texture(t.welcome_banner_png, s->welcome_w, s->welcome_h);
    } else if (!s->payload->welcome_png.empty()) {
        // Legacy fallback.
        std::vector<uint8_t> png(s->payload->welcome_png.begin(),
                                 s->payload->welcome_png.end());
        s->welcome_tex = load_png_texture(png, s->welcome_w, s->welcome_h);
    }
    s->sidebar_tex     = load_png_texture(t.sidebar_image_png,    s->sidebar_w,    s->sidebar_h);
    s->background_tex   = load_png_texture(t.background_image_png, s->background_w, s->background_h);
    s->footer_tex      = load_png_texture(t.footer_logo_png,      s->footer_w,     s->footer_h);
}

static inline ImVec4 rgba(const uint8_t c[4]) {
    return ImVec4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
}

void wizard_apply_theme(const WizardState* s) {
    const UiTheme& t = s->payload->theme;
    ImGuiStyle& style = ImGui::GetStyle();

    ImGui::StyleColorsDark();  // base, then override below

    style.Colors[ImGuiCol_WindowBg]     = rgba(t.background);
    style.Colors[ImGuiCol_ChildBg]     = rgba(t.panel);
    style.Colors[ImGuiCol_PopupBg]     = rgba(t.panel);
    style.Colors[ImGuiCol_FrameBg]     = rgba(t.panel);
    style.Colors[ImGuiCol_Text]        = rgba(t.text);
    style.Colors[ImGuiCol_TextDisabled] = rgba(t.text_dim);
    style.Colors[ImGuiCol_Button]     = rgba(t.button);
    style.Colors[ImGuiCol_ButtonHovered] = rgba(t.primary_hover);
    style.Colors[ImGuiCol_ButtonActive]  = rgba(t.primary);
    style.Colors[ImGuiCol_Border]      = rgba(t.border);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_FrameBgHovered] = rgba(t.primary_hover);
    style.Colors[ImGuiCol_FrameBgActive]  = rgba(t.primary);

    style.FrameRounding = 4.0f;
    style.WindowRounding = 0.0f;
    style.WindowPadding = ImVec2((float)t.margin, (float)t.margin);
    style.ItemSpacing   = ImVec2((float)t.margin, (float)t.item_spacing_y);
    style.FramePadding  = ImVec2(8.0f, 6.0f);
    // Font is loaded in main.cpp before backend init (msyh.ttc for CJK).
}

// ---------------------------------------------------------------------------
// Shared page chrome
// ---------------------------------------------------------------------------

static void begin_page_window(WizardState* s, const char* title) {
    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Full-window background image (if theme has one).
    if (s->background_tex) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddImage((ImTextureID)(intptr_t)s->background_tex,
                     vp->WorkPos,
                     ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                            vp->WorkPos.y + vp->WorkSize.y));
    }

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin(title, nullptr, flags);
}

static void page_footer_buttons(WizardState* s,
                                bool can_prev, bool can_next,
                                const char* next_label,
                                WizardPage next_page) {
    // Push the button row to the bottom with vertical spacer.
    ImGui::NewLine();
    ImGui::NewLine();

    // Right-align the button row.
    float button_w = 100.0f;
    float spacing  = 8.0f;
    float total    = button_w * 3 + spacing * 2;
    float avail_x  = ImGui::GetContentRegionMax().x - ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(avail_x - total);

    if (ImGui::Button(atu::i18n("btn_cancel").c_str(), ImVec2(button_w, 0))) s->request_exit = true;
    ImGui::SameLine();
    bool prev_enabled = can_prev;
    if (!prev_enabled) ImGui::BeginDisabled();
    if (ImGui::Button(atu::i18n("btn_back").c_str(), ImVec2(button_w, 0)) && prev_enabled) {
        // Simple back mapping: welcome<-license<-dir.
        if (s->page == WizardPage::License)       s->page = WizardPage::Welcome;
        else if (s->page == WizardPage::Dir) {
            if (s->payload->license_text.empty()) s->page = WizardPage::Welcome;
            else                                 s->page = WizardPage::License;
        }
    }
    if (!prev_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!can_next) ImGui::BeginDisabled();
    if (ImGui::Button(next_label, ImVec2(button_w, 0)) && can_next) {
        s->page = next_page;
    }
    if (!can_next) ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

// v0.20: lazily populate available_components from payload.config.
// The packer emits "Components=a,b,c" and "ComponentsDesc=a:desc|b:desc|...".
static void ensure_components_loaded(WizardState* s) {
    if (s->components_loaded) return;
    s->components_loaded = true;
    if (!s->payload) return;

    std::string names = s->payload->get("Components", "");
    if (names.empty()) return;

    // Split names by comma.
    std::vector<std::string> name_list;
    {
        size_t start = 0;
        for (size_t i = 0; i <= names.size(); ++i) {
            if (i == names.size() || names[i] == ',') {
                name_list.push_back(names.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    // Split descs by '|'; each token is "name:description".
    std::vector<std::string> desc_list;
    {
        std::string descs = s->payload->get("ComponentsDesc", "");
        size_t start = 0;
        for (size_t i = 0; i <= descs.size(); ++i) {
            if (i == descs.size() || descs[i] == '|') {
                desc_list.push_back(descs.substr(start, i - start));
                start = i + 1;
            }
        }
    }

    for (size_t i = 0; i < name_list.size(); ++i) {
        std::string desc;
        if (i < desc_list.size()) {
            auto colon = desc_list[i].find(':');
            if (colon != std::string::npos) desc = desc_list[i].substr(colon + 1);
        }
        s->available_components.emplace_back(name_list[i], desc);
        s->component_selected.push_back(1);  // default: all checked
    }
    // Apply the default install type (typical) once.
    // v0.20: optional preset overrides from config (forward-compatible).
    //   TypeTypical=a,b   -> components checked in typical mode.
    //   TypeComplete=all  -> complete mode checks everything (default).
    std::string typ = s->payload->get("TypeTypical", "");
    if (!typ.empty()) {
        std::vector<char> preset(s->component_selected.size(), 0);
        size_t start = 0;
        for (size_t i = 0; i <= typ.size(); ++i) {
            if (i == typ.size() || typ[i] == ',') {
                std::string cn = typ.substr(start, i - start);
                for (size_t k = 0; k < s->available_components.size(); ++k)
                    if (s->available_components[k].first == cn) preset[k] = 1;
                start = i + 1;
            }
        }
        s->component_selected = preset;
    }
}

// v0.20: apply an install-type preset to component_selected.
//   type 0 = typical  (preset; default = all, or TypeTypical override)
//   type 1 = complete (all)
//   type 2 = custom   (leave current selection, let user tweak)
static void apply_install_type(WizardState* s, int type) {
    s->install_type = type;
    if (type == 1) {
        for (auto& c : s->component_selected) c = 1;
    } else if (type == 0) {
        // Typical preset: re-run the same rule as initial load.
        std::string typ = s->payload ? s->payload->get("TypeTypical", "") : "";
        if (typ.empty()) {
            for (auto& c : s->component_selected) c = 1;
        } else {
            for (auto& c : s->component_selected) c = 0;
            size_t start = 0;
            for (size_t i = 0; i <= typ.size(); ++i) {
                if (i == typ.size() || typ[i] == ',') {
                    std::string cn = typ.substr(start, i - start);
                    for (size_t k = 0; k < s->available_components.size(); ++k)
                        if (s->available_components[k].first == cn)
                            s->component_selected[k] = 1;
                    start = i + 1;
                }
            }
        }
    }
    // type == 2 (custom): do not touch component_selected.
}

// Build the active_components set from component_selected (called when leaving
// the Components page).
static void resolve_active_components(WizardState* s) {
    s->active_components.clear();
    for (size_t i = 0; i < s->available_components.size(); ++i) {
        if (s->component_selected[i])
            s->active_components.insert(s->available_components[i].first);
    }
}

// v0.24: build the active_tasks set from payload->tasks[i].checked. The
// Components page checkboxes mutate TaskEntry::checked in place; in silent
// mode the parser default (respecting [Tasks] Flags: unchecked) is already
// in that field. Called at the top of page_progress (same place components
// are resolved) so it works whether or not the user visited the Components
// page.
static void resolve_active_tasks(WizardState* s) {
    s->active_tasks.clear();
    if (!s->payload) return;
    for (const auto& t : s->payload->tasks) {
        if (t.checked) s->active_tasks.insert(t.name);
    }
}

// v0.24: decide whether a built-in shortcut task is enabled.
//   * If the [Tasks] section declares a task named `name`, its state is taken
//     from `active` (the set of checked tasks resolved at install start).
//   * If no such task is declared, default to true -- this preserves the
//     legacy behavior where the installer always created Start Menu + Desktop
//     shortcuts for scripts that did not opt into task gating.
// Strip a single pair of surrounding double-quotes, if present. The compiler
// side lexer does not strip quotes from [Tasks] Name values, so TaskEntry::name
// arrives as e.g. "\"CreateDesktopIcon\""; built-in gating compares against the
// bare name.
static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static bool task_enabled(const ParsedPayload& p,
                         const std::set<std::string>& active,
                         const char* name) {
    bool declared = false;
    for (const auto& t : p.tasks) {
        if (strip_quotes(t.name) == name) { declared = true; break; }
    }
    if (!declared) return true;
    // active_tasks is built from the same quoted TaskEntry::name values.
    for (const auto& n : active) {
        if (strip_quotes(n) == name) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// v0.20: [Setup] page flags (DisableWelcomePage etc.)
// ---------------------------------------------------------------------------

static bool cfg_flag_is_on(const WizardState* s, const char* key) {
    std::string v = s->payload->get(key, "0");
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    return v == "1" || v == "yes" || v == "true" || v == "on";
}

// Compute the natural next page after Welcome (mirrors page_welcome's choice).
static WizardPage natural_next_after_welcome(const WizardState* s) {
    if (!s->payload->license_text.empty()) return WizardPage::License;
    return s->available_components.empty() ? WizardPage::Dir : WizardPage::Components;
}

void wizard_apply_page_flags(WizardState* s) {
    if (!s || !s->payload) return;
    // Make sure component list is populated so the Welcome-skip target is correct.
    ensure_components_loaded(s);

    if (cfg_flag_is_on(s, "DisableWelcomePage")) {
        s->page = natural_next_after_welcome(s);
    }
    // DisableFinishedPage: after install completes the wizard exits immediately
    // instead of showing the Finish page. Honored in page_progress.
}

static void page_welcome(WizardState* s) {
    const auto& cfg = s->payload->config;
    std::string app   = s->payload->get("AppName", "ApTgeu App");
    std::string ver   = s->payload->get("AppVersion", "");
    std::string pub   = s->payload->get("Publisher", "");
    const UiTheme& t = s->payload->theme;

    // Top: welcome banner (full content width, preserve aspect).
    if (s->welcome_tex) {
        float max_w = ImGui::GetContentRegionAvail().x;
        float aspect = (float)s->welcome_h / (float)s->welcome_w;
        float w = max_w;
        float h = max_w * aspect;
        if (h > 200.0f) { h = 200.0f; w = h / aspect; }
        ImGui::Image((ImTextureID)(intptr_t)s->welcome_tex, ImVec2(w, h));
        ImGui::Separator();
    }

    // Left: sidebar image (narrow column). Right: app info.
    float sidebar_h = ImGui::GetContentRegionAvail().y - 60.0f;
    if (sidebar_h < 80.0f) sidebar_h = 80.0f;

    if (s->sidebar_tex) {
        float sw = (float)t.sidebar_width;
        float aspect = (float)s->sidebar_h / (float)s->sidebar_w;
        float sh = sw * aspect;
        if (sh > sidebar_h) sh = sidebar_h;
        ImGui::Image((ImTextureID)(intptr_t)s->sidebar_tex, ImVec2(sw, sh));
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::NewLine();
    ImGui::TextUnformatted((atu::i18n("welcome_title") + app).c_str());
    if (!ver.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(("  " + ver).c_str()); }
    if (!pub.empty()) { ImGui::NewLine(); ImGui::TextUnformatted((std::string("Publisher: ") + pub).c_str()); }
    ImGui::NewLine();
    ImGui::TextWrapped(atu::i18n("welcome_desc").c_str());
    ImGui::EndGroup();

    ImGui::NewLine();
    WizardPage next = s->payload->license_text.empty()
        ? (s->available_components.empty() ? WizardPage::Dir : WizardPage::Components)
        : WizardPage::License;
    page_footer_buttons(s, false, true, atu::i18n("btn_next").c_str(), next);
}

static void page_license(WizardState* s) {
    ImGui::SeparatorText(atu::i18n("license_title").c_str());
    ImGui::NewLine();
    ImGui::TextWrapped(atu::i18n("license_desc").c_str());
    ImGui::NewLine();

    // Read-only multiline. Reserve ~110px for checkbox + footer buttons.
    ImGui::InputTextMultiline("##license",
                              (char*)s->payload->license_text.data(),
                              s->payload->license_text.size() + 1,
                              ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 110.0f),
                              ImGuiInputTextFlags_ReadOnly);

    ImGui::Checkbox(atu::i18n("license_accept").c_str(), &s->agree_license);

    ImGui::NewLine();
    page_footer_buttons(s, true, s->agree_license, atu::i18n("btn_next").c_str(), WizardPage::Dir);
}

// v0.19: Readme page
static void page_readme(WizardState* s) {
    ImGui::SeparatorText("Readme");
    ImGui::BeginChild("readme_scroll", ImVec2(0, 300), true);
    ImGui::TextUnformatted(s->readme_text.c_str());
    ImGui::EndChild();
    page_footer_buttons(s, true, true, atu::i18n("btn_next").c_str(), WizardPage::Dir);
}

// v0.19: Ready page (install summary)
static void page_ready(WizardState* s) {
    ImGui::SeparatorText("准备安装");
    ImGui::Text("安装目录: %S", s->install_dir_buf);
    ImGui::Text("语言: %s", atu::i18n("welcome_title").c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("点击\"安装\"开始复制文件到目标目录。");
    page_footer_buttons(s, true, true, "安装", WizardPage::Preparing);
}

// v0.19: Preparing page - kill running app
static void page_preparing(WizardState* s) {
    ImGui::SeparatorText("准备");
    ImGui::Text("正在检查是否有程序正在运行...");
    // Find FileTgie.exe process
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    bool found = false;
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"FileTgie.exe") == 0) { found = true; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (!found) {
        // no process, auto advance
        s->page = WizardPage::Progress;
        return;
    }
    ImGui::TextWrapped("检测到 FileTgie.exe 正在运行，需要关闭才能继续安装。");
    if (ImGui::Button("关闭并继续")) {
        // try to terminate
        HANDLE snap2 = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap2, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"FileTgie.exe") == 0) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); CloseHandle(h); }
                    break;
                }
            } while (Process32NextW(snap2, &pe));
        }
        CloseHandle(snap2);
        s->page = WizardPage::Progress;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消安装")) { s->page = WizardPage::Ready; }
}

static void page_dir(WizardState* s) {
    ImGui::SeparatorText(atu::i18n("dir_title").c_str());
    ImGui::NewLine();
    ImGui::TextWrapped(atu::i18n("dir_desc").c_str());
    ImGui::NewLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Text(atu::i18n("dir_label").c_str());
    ImGui::SameLine();
    ImGui::PushItemWidth(-130.0f);
    ImGui::InputText("##dir", s->install_dir_buf, sizeof(s->install_dir_buf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(atu::i18n("dir_browse").c_str(), ImVec2(120, 0))) {
        BROWSEINFOW bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = nullptr;
        bi.lpszTitle = atu::i18n_w("dir_title").c_str();
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH];
            if (::SHGetPathFromIDListW(pidl, path)) {
                int n = ::WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                              s->install_dir_buf, sizeof(s->install_dir_buf),
                                              nullptr, nullptr);
                (void)n;
            }
            ::CoTaskMemFree(pidl);
        }
    }

    ImGui::NewLine();
    ImGui::NewLine();

    // v0.2.5: disk space check - required = sum(file sizes) * 1.10.
    bool disk_ok = true;
    {
        std::wstring wdir = utf8_to_wide(s->install_dir_buf);
        double required_mb = compute_required_mb(*s->payload);
        ULARGE_INTEGER free_bytes;
        if (!wdir.empty() && get_free_bytes(wdir, &free_bytes)) {
            double free_mb = (double)free_bytes.QuadPart / (1024.0 * 1024.0);
            wchar_t line[256];
            _swprintf(line, atu::i18n_w("disk_space_status").c_str(), free_mb, required_mb);
            ImGui::Text("%s", wide_to_utf8(line).c_str());
            if (free_mb < required_mb) {
                wchar_t warn[256];
                _swprintf(warn, atu::i18n_w("disk_space_warning").c_str(), required_mb, free_mb);
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s",
                    wide_to_utf8(warn).c_str());
                disk_ok = false;
            }
        }
    }

    bool ok = s->install_dir_buf[0] != '\0' && disk_ok;
    WizardPage next = s->available_components.empty() ? WizardPage::Progress : WizardPage::Components;
    std::string next_label = (next == WizardPage::Progress) ? atu::i18n("btn_install") : atu::i18n("btn_next");
    page_footer_buttons(s, true, ok, next_label.c_str(), next);
}

static void page_components(WizardState* s) {
    ensure_components_loaded(s);

    ImGui::SeparatorText(atu::i18n("components_title").c_str());
    ImGui::NewLine();
    ImGui::TextWrapped(atu::i18n("components_desc").c_str());
    ImGui::NewLine();

    // v0.20: install-type radio buttons (typical / complete / custom).
    ImGui::TextUnformatted("安装类型:");
    bool changed = false;
    if (ImGui::RadioButton("典型安装 (推荐)", s->install_type == 0)) {
        apply_install_type(s, 0);
        changed = true;
    }
    if (ImGui::RadioButton("完整安装", s->install_type == 1)) {
        apply_install_type(s, 1);
        changed = true;
    }
    if (ImGui::RadioButton("自定义安装", s->install_type == 2)) {
        apply_install_type(s, 2);
        changed = true;
    }
    (void)changed;

    ImGui::NewLine();
    ImGui::Separator();
    ImGui::NewLine();

    // Component checkboxes: only interactive in custom mode.
    bool manual = (s->install_type == 2);
    if (!manual) ImGui::BeginDisabled();
    for (size_t i = 0; i < s->available_components.size(); ++i) {
        const auto& [name, desc] = s->available_components[i];
        std::string label = desc.empty() ? name : (desc + " (" + name + ")");
        bool checked = s->component_selected[i] != 0;
        if (ImGui::Checkbox(label.c_str(), &checked)) {
            s->component_selected[i] = checked ? 1 : 0;
        }
    }
    if (!manual) ImGui::EndDisabled();

    // v0.14: tasks checkboxes
    if (!s->payload->tasks.empty()) {
        ImGui::NewLine();
        ImGui::Separator();
        ImGui::NewLine();
        for (size_t i = 0; i < s->payload->tasks.size(); ++i) {
            auto& t = const_cast<std::vector<TaskEntry>&>(s->payload->tasks);
            std::string label = t[i].description.empty() ? t[i].name : t[i].description;
            ImGui::Checkbox(label.c_str(), &t[i].checked);
        }
    }

    ImGui::NewLine();
    page_footer_buttons(s, true, true, atu::i18n("btn_next").c_str(), WizardPage::Progress);
}

// v0.21: force-kill every process whose exe name matches (Inno
// /FORCECLOSEAPPLICATIONS / /CLOSEAPPLICATIONS). Best-effort.
static void kill_process_by_exe_name(const std::wstring& exe_name) {
    if (exe_name.empty()) return;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name.c_str()) == 0) {
                HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    ::TerminateProcess(h, 0);
                    ::WaitForSingleObject(h, 3000);
                    ::CloseHandle(h);
                }
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
}

// v0.23: [InstallService] - create Windows services via SCM.
// Wire: SvcN = name|display_name|description|start_type|executable
// {app} in executable is substituted by install_dir_utf8.
// Errors are logged but do NOT abort install. Returns the list of service
// names that were successfully created/updated (caller writes them into
// unins000_extra.ini so the standalone uninstaller can DeleteService).
static std::vector<std::wstring> install_services_from_payload(
        const ParsedPayload& p, const std::string& install_dir_utf8) {
    std::vector<std::wstring> created;
    int svc_count = 0;
    try { svc_count = std::stoi(p.get("SvcCount", "0")); } catch (...) {}
    if (svc_count <= 0) return created;

    SC_HANDLE scm = ::OpenSCManagerW(nullptr, L"ServicesActive",
                                     SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        atu::log_info("install: OpenSCManagerW failed (err=" +
                      std::to_string(::GetLastError()) + ") - services skipped");
        return created;
    }
    for (int i = 0; i < svc_count; ++i) {
        std::string val = p.get("Svc" + std::to_string(i), "");
        if (val.empty()) continue;
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t j = 0; j <= val.size(); ++j) {
            if (j == val.size() || val[j] == '|') {
                parts.push_back(val.substr(start, j - start));
                start = j + 1;
            }
        }
        if (parts.size() < 5) continue;
        std::string name = parts[0];
        std::string disp = parts[1];
        std::string desc = parts[2];
        std::string stype = parts[3];
        std::string exe   = parts[4];
        size_t ap = exe.find("{app}");
        if (ap != std::string::npos)
            exe = exe.substr(0, ap) + install_dir_utf8 + exe.substr(ap + 5);

        DWORD dwStartType = SERVICE_DEMAND_START;
        if (stype == "auto" || stype == "SERVICE_AUTO_START") dwStartType = SERVICE_AUTO_START;
        else if (stype == "disabled" || stype == "SERVICE_DISABLED") dwStartType = SERVICE_DISABLED;

        std::wstring wname = utf8_to_wide(name);
        std::wstring wdisp = utf8_to_wide(disp.empty() ? name : disp);
        std::wstring wexe  = utf8_to_wide(exe);
        std::wstring imagePath = L"\"" + wexe + L"\"";
        SC_HANDLE svc = ::CreateServiceW(scm,
                                         wname.c_str(),
                                         wdisp.c_str(),
                                         SERVICE_ALL_ACCESS,
                                         SERVICE_WIN32_OWN_PROCESS,
                                         dwStartType,
                                         SERVICE_ERROR_NORMAL,
                                         imagePath.c_str(),
                                         nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            DWORD e = ::GetLastError();
            if (e == ERROR_SERVICE_EXISTS) {
                atu::log_info("install: service already exists, updating config: " + name);
                svc = ::OpenServiceW(scm, wname.c_str(), SERVICE_CHANGE_CONFIG);
                if (svc) {
                    ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, dwStartType,
                                         SERVICE_ERROR_NORMAL, imagePath.c_str(),
                                         nullptr, nullptr, nullptr, nullptr, nullptr,
                                         wdisp.c_str());
                }
            } else {
                atu::log_info("install: CreateServiceW failed (" + std::to_string(e) +
                              ") for " + name);
            }
        }
        if (svc) {
            if (!desc.empty()) {
                SERVICE_DESCRIPTIONW sd;
                std::wstring wdesc = utf8_to_wide(desc);
                sd.lpDescription = const_cast<LPWSTR>(wdesc.c_str());
                ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);
            }
            ::CloseServiceHandle(svc);
            created.push_back(wname);
        }
    }
    ::CloseServiceHandle(scm);
    return created;
}

// ---------------------------------------------------------------------------
// v0.24: [Run] section - install-time command execution.
// Wire: InstRun{i} = filename|parameters|workingdir|flags|description
// {app} in filename / workingdir is substituted with install_dir at runtime.
// Flags honoured here:
//   postinstall               - only run when the Finish-page checkbox is ticked
//   runhidden                 - launch with SW_HIDE / CREATE_NO_WINDOW
//   skipifsourcedoesntexist   - silently skip if the target file does not exist
//   nowait                    - do not wait for the child process
// ---------------------------------------------------------------------------

struct InstallRunEntry {
    std::string filename;
    std::string parameters;
    std::string workingdir;
    std::string flags;
    std::string description;
    bool has_postinstall = false;
    bool has_runhidden   = false;
    bool has_skipifmiss  = false;
    bool has_nowait      = false;
};

static std::vector<InstallRunEntry> load_install_run_entries(const ParsedPayload& p) {
    std::vector<InstallRunEntry> out;
    int n = 0;
    try { n = std::stoi(p.get("InstRunCount", "0")); } catch (...) {}
    for (int i = 0; i < n; ++i) {
        std::string val = p.get("InstRun" + std::to_string(i), "");
        if (val.empty()) continue;
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t j = 0; j <= val.size(); ++j) {
            if (j == val.size() || val[j] == '|') {
                parts.push_back(val.substr(start, j - start));
                start = j + 1;
            }
        }
        InstallRunEntry e;
        if (parts.size() > 0) e.filename    = parts[0];
        if (parts.size() > 1) e.parameters  = parts[1];
        if (parts.size() > 2) e.workingdir  = parts[2];
        if (parts.size() > 3) e.flags       = parts[3];
        if (parts.size() > 4) e.description = parts[4];
        auto has_flag = [](const std::string& hay, const char* needle) {
            std::string cur;
            for (char c : hay) {
                if (c == ' ' || c == '\t' || c == ',') {
                    if (cur == needle) return true;
                    cur.clear();
                } else cur.push_back(c);
            }
            return cur == needle;
        };
        e.has_postinstall = has_flag(e.flags, "postinstall");
        e.has_runhidden   = has_flag(e.flags, "runhidden");
        e.has_skipifmiss  = has_flag(e.flags, "skipifsourcedoesntexist");
        e.has_nowait      = has_flag(e.flags, "nowait");
        out.push_back(std::move(e));
    }
    return out;
}

static std::string substitute_app_utf8(const std::string& s, const std::string& app) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (i + 4 < s.size() && s[i]=='{' && s[i+1]=='a' && s[i+2]=='p' &&
            s[i+3]=='p' && s[i+4]=='}') {
            out += app;
            i += 5;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

static bool execute_install_run_entry(const InstallRunEntry& e,
                                      const std::string& install_dir_utf8) {
    std::string fn     = substitute_app_utf8(e.filename, install_dir_utf8);
    std::string wd     = substitute_app_utf8(e.workingdir, install_dir_utf8);
    std::string params = e.parameters;

    if (e.has_skipifmiss) {
        std::wstring wfn = utf8_to_wide(fn);
        DWORD attr = ::GetFileAttributesW(wfn.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            atu::log_info("[Run] skip (source missing): " + fn);
            return false;
        }
    }

    std::wstring wfn = utf8_to_wide(fn);
    std::wstring wparams = utf8_to_wide(params);
    std::wstring wcmd = L"\"" + wfn + L"\"";
    if (!wparams.empty()) wcmd += L" " + wparams;

    std::wstring wwd = wd.empty() ? utf8_to_wide(install_dir_utf8)
                                  : utf8_to_wide(wd);

    STARTUPINFOW si; ::ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ::ZeroMemory(&pi, sizeof(pi));
    if (e.has_runhidden) {
        si.dwFlags     |= STARTF_USESHOWWINDOW;
        si.wShowWindow  = SW_HIDE;
    }
    std::vector<wchar_t> mut(wcmd.begin(), wcmd.end());
    mut.push_back(L'\0');
    DWORD create_flags = e.has_runhidden ? CREATE_NO_WINDOW : 0;
    BOOL ok = ::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                               create_flags, nullptr, wwd.c_str(), &si, &pi);
    if (!ok) {
        atu::log_warn("[Run] CreateProcessW failed err=" +
                      std::to_string(::GetLastError()) + " cmd=" + fn);
        return false;
    }
    atu::log_info("[Run] launched: " + fn + (params.empty() ? "" : (" " + params)));
    if (e.has_nowait) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        ::WaitForSingleObject(pi.hProcess, 30000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    return true;
}

// Run [Run] entries matching `want_postinstall`. Non-postinstall entries run
// immediately after install; postinstall entries run when the user ticks the
// Finish-page checkbox and presses Finish.
static void run_install_entries(WizardState* s, bool want_postinstall) {
    auto entries = load_install_run_entries(*s->payload);
    std::string dir_utf8 = s->install_dir_buf;
    size_t pi = 0;
    for (const auto& e : entries) {
        if (e.has_postinstall != want_postinstall) continue;
        if (want_postinstall) {
            if (!s->postinstall_state_loaded) { ++pi; continue; }
            if (pi >= s->postinstall_checked.size() || !s->postinstall_checked[pi]) {
                atu::log_info("[Run] postinstall entry not ticked, skipped.");
                ++pi;
                continue;
            }
            ++pi;
        }
        execute_install_run_entry(e, dir_utf8);
    }
}

static void ensure_postinstall_checkboxes(WizardState* s) {
    if (s->postinstall_state_loaded) return;
    s->postinstall_state_loaded = true;
    auto entries = load_install_run_entries(*s->payload);
    for (const auto& e : entries) {
        if (!e.has_postinstall) continue;
        std::string label = e.description;
        if (label.empty()) {
            std::string fn = e.filename;
            size_t slash = fn.find_last_of("\\/");
            if (slash != std::string::npos) fn = fn.substr(slash + 1);
            label = "Run " + fn;
        }
        s->postinstall_labels.push_back(label);
        s->postinstall_checked.push_back(1);
    }
}

// Finalize after all files written: shortcuts + dat + self-copy + registry.
static bool finalize_install(WizardState* s, std::string& err) {
    std::wstring dir = utf8_to_wide(s->install_dir_buf);
    std::wstring app_name = utf8_to_wide(s->payload->get("AppName", "App"));

    // Create Start Menu / Desktop / Quick Launch shortcuts; add them to the
    // cleanup list. Assume main exe is <installDir>\<AppName>.exe (MVP
    // convention).
    // v0.21: /NOICONS skips all shortcut creation; /GROUP=... names the folder.
    // v0.24: each shortcut is gated by its built-in task (CreateDesktopIcon /
    // CreateProgramMenuIcon / CreateQuickLaunchIcon). If the script does not
    // declare that task, the legacy default (desktop+progmenu on) applies.
    std::wstring exe_rel = app_name + L".exe";
    if (!s->no_icons) {
        bool want_desktop   = task_enabled(*s->payload, s->active_tasks, "CreateDesktopIcon");
        bool want_progmenu  = task_enabled(*s->payload, s->active_tasks, "CreateProgramMenuIcon");
        bool want_quick     = task_enabled(*s->payload, s->active_tasks, "CreateQuickLaunchIcon");
        atu::log_info(std::string("install: shortcut tasks desktop=") +
                      (want_desktop?"1":"0") + " progmenu=" + (want_progmenu?"1":"0") +
                      " quicklaunch=" + (want_quick?"1":"0"));
        auto shortcuts = create_shortcuts(dir, app_name, exe_rel, s->group_name,
                                          want_desktop, want_progmenu, want_quick);
        for (auto& p : shortcuts) s->written_paths.push_back(p);
    } else {
        atu::log_info("install: /NOICONS set, skipping shortcut creation");
    }

    // v0.16: [Icons] section - create custom shortcuts via IShellLinkW
    {
        int icon_count = 0;
        try { icon_count = std::stoi(s->payload->get("IconCount", "0")); } catch (...) {}
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (int i = 0; i < icon_count; ++i) {
            std::string val = s->payload->get("Icon" + std::to_string(i), "");
            if (val.empty()) continue;
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t j = 0; j <= val.size(); ++j) {
                if (j == val.size() || val[j] == '|') {
                    parts.push_back(val.substr(start, j - start));
                    start = j + 1;
                }
            }
            if (parts.size() < 6) continue;
            // substitute {app} in name and filename
            std::string name = parts[0];
            std::string fname = parts[1];
            for (auto* str : {&name, &fname, &parts[3], &parts[5]}) {
                size_t ap = str->find("{app}");
                if (ap != std::string::npos)
                    *str = str->substr(0, ap) + s->install_dir_buf + str->substr(ap + 5);
            }
            // substitute {userdesktop}
            for (auto* str : {&name}) {
                size_t ap = str->find("{userdesktop}");
                if (ap != std::string::npos) {
                    wchar_t buf[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf);
                    std::wstring wd(buf);
                    std::string wd8(wd.begin(), wd.end());
                    *str = str->substr(0, ap) + wd8 + str->substr(ap + 13);
                }
                size_t ag = str->find("{group}");
                if (ag != std::string::npos) {
                    wchar_t buf[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, buf);
                    std::wstring wd(buf);
                    std::string wd8(wd.begin(), wd.end());
                    *str = str->substr(0, ag) + wd8 + str->substr(ag + 7);
                }
            }
            std::wstring wpath = utf8_to_wide(name);
            std::wstring wtarget = utf8_to_wide(fname);
            // ensure parent dir
            CreateDirectoryW(wpath.substr(0, wpath.find_last_of(L"\\/")).c_str(), nullptr);
            // IShellLink
            IShellLinkW* sl = nullptr;
            if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl) == S_OK) {
                sl->SetPath(wtarget.c_str());
                if (!parts[4].empty()) sl->SetArguments(utf8_to_wide(parts[4]).c_str());
                if (!parts[5].empty()) sl->SetWorkingDirectory(utf8_to_wide(parts[5]).c_str());
                if (!parts[3].empty()) sl->SetDescription(utf8_to_wide(parts[3]).c_str());
                IPersistFile* pf = nullptr;
                if (sl->QueryInterface(IID_IPersistFile, (void**)&pf) == S_OK) {
                    pf->Save(wpath.c_str(), TRUE);
                    pf->Release();
                }
                sl->Release();
                s->written_paths.push_back(wpath);
            }
        }
        CoUninitialize();
    }

    // v0.19: [Dirs] - create empty directories
    {
        int dir_count = 0;
        try { dir_count = std::stoi(s->payload->get("DirCount", "0")); } catch (...) {}
        for (int i = 0; i < dir_count; ++i) {
            std::string name = s->payload->get("Dir" + std::to_string(i), "");
            if (name.empty()) continue;
            size_t ap = name.find("{app}");
            if (ap != std::string::npos)
                name = name.substr(0, ap) + s->install_dir_buf + name.substr(ap + 5);
            std::wstring wdir = utf8_to_wide(name);
            CreateDirectoryW(wdir.c_str(), nullptr);
        }
    }

    // v0.14: write registry entries from config "Reg0=HKCU|subkey|type|name|data"
    {
        int reg_count = 0;
        try { reg_count = std::stoi(s->payload->get("RegCount", "0")); } catch (...) {}
        for (int i = 0; i < reg_count; ++i) {
            std::string key = "Reg" + std::to_string(i);
            std::string val = s->payload->get(key, "");
            if (val.empty()) continue;
            // split by |
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t j = 0; j <= val.size(); ++j) {
                if (j == val.size() || val[j] == '|') {
                    parts.push_back(val.substr(start, j - start));
                    start = j + 1;
                }
            }
            if (parts.size() < 5) continue;
            HKEY hRoot = (parts[0] == "HKLM") ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
            std::wstring wsub = utf8_to_wide(parts[1]);
            std::wstring wname = utf8_to_wide(parts[3]);
            std::wstring wdata = utf8_to_wide(parts[4]);
            HKEY hKey;
            if (RegCreateKeyExW(hRoot, wsub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
                if (parts[2] == "dword") {
                    DWORD dw = 0;
                    try { dw = std::stoul(parts[4]); } catch (...) {}
                    RegSetValueExW(hKey, wname.c_str(), 0, REG_DWORD, (BYTE*)&dw, sizeof(dw));
                } else {
                    RegSetValueExW(hKey, wname.c_str(), 0, REG_SZ, (const BYTE*)wdata.c_str(),
                                   (DWORD)((wdata.size() + 1) * sizeof(wchar_t)));
                }
                RegCloseKey(hKey);
            }
        }
    }

    // v0.15: write INI entries from config "Ini0=filename|section|key|value"
    {
        int ini_count = 0;
        try { ini_count = std::stoi(s->payload->get("IniCount", "0")); } catch (...) {}
        for (int i = 0; i < ini_count; ++i) {
            std::string val = s->payload->get("Ini" + std::to_string(i), "");
            if (val.empty()) continue;
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t j = 0; j <= val.size(); ++j) {
                if (j == val.size() || val[j] == '|') {
                    parts.push_back(val.substr(start, j - start));
                    start = j + 1;
                }
            }
            if (parts.size() < 4) continue;
            // substitute {app}
            std::string fn = parts[0];
            size_t app_pos = fn.find("{app}");
            if (app_pos != std::string::npos)
                fn = fn.substr(0, app_pos) + s->install_dir_buf + fn.substr(app_pos + 5);
            WritePrivateProfileStringW(
                utf8_to_wide(parts[1]).c_str(),
                utf8_to_wide(parts[2]).c_str(),
                utf8_to_wide(parts[3]).c_str(),
                utf8_to_wide(fn).c_str());
        }
    }

    // v0.15: InstallDelete entries (delete files/dirs before install)
    {
        int del_count = 0;
        try { del_count = std::stoi(s->payload->get("InstDelCount", "0")); } catch (...) {}
        for (int i = 0; i < del_count; ++i) {
            std::string val = s->payload->get("InstDel" + std::to_string(i), "");
            if (val.empty()) continue;
            size_t sep = val.find('|');
            if (sep == std::string::npos) continue;
            std::string type = val.substr(0, sep);
            std::string name = val.substr(sep + 1);
            size_t app_pos = name.find("{app}");
            if (app_pos != std::string::npos)
                name = name.substr(0, app_pos) + s->install_dir_buf + name.substr(app_pos + 5);
            std::wstring wname = utf8_to_wide(name);
            DWORD attr = GetFileAttributesW(wname.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) continue;
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(wname.c_str()); // best effort, empty dir only
            } else {
                DeleteFileW(wname.c_str());
            }
        }
    }

    // v0.23: [InstallService] - create Windows services via SCM.
    std::vector<std::wstring> installed_services =
        install_services_from_payload(*s->payload, s->install_dir_buf);

    if (!write_unins_dat(dir, s->written_paths)) {
        err = "写 unins000.dat 失败";
        return false;
    }

    // Write the standalone uninstaller PE (from payload APTGEU::unins.exe).
    if (!s->payload->unins_exe_data.empty()) {
        if (!install_unins_exe(s->payload->unins_exe_data, dir)) {
            err = "写 unins000.exe 失败";
            return false;
        }
    } else {
        // Fallback: copy self (legacy path if compiler didn't bundle unins.exe).
        std::wstring self_dest = dir + L"\\unins000.exe";
        if (!copy_self_to(self_dest)) {
            err = "复制 unins000.exe 失败";
            return false;
        }
    }

    // v0.12: Write uninstall theme ini.
    if (!s->payload->unins_ini_data.empty()) {
        install_unins_ini(s->payload->unins_ini_data, dir);
    }

    // v0.23: apply [Registry] entries (write real keys/values), and collect
    // the wire lines so the standalone uninstaller can delete them later.
    std::string reg_unins_lines;
    std::string ini_unins_lines;
    {
        std::wstring dir_w = utf8_to_wide(s->install_dir_buf);
        if (!apply_registry_entries(*s->payload, dir_w, reg_unins_lines)) {
            atu::log_warn("install: one or more [Registry] entries failed to write");
        }
        // v0.24: apply [INI] entries (WritePrivateProfileStringW).
        if (!apply_ini_entries(*s->payload, dir_w, ini_unins_lines)) {
            atu::log_warn("install: one or more [INI] entries failed to write");
        }
    }

    // v0.15: write unins000_extra.ini with UninstallRun + UninstallDelete entries
    {
        std::string extra;
        int urc = 0, udc = 0;
        try { urc = std::stoi(s->payload->get("UninsRunCount", "0")); } catch (...) {}
        try { udc = std::stoi(s->payload->get("UninsDelCount", "0")); } catch (...) {}
        atu::log_info("extra.ini: urc=" + std::to_string(urc) +
                      " UninsRun0='" + s->payload->get("UninsRun0","") + "'");
        // v0.19: collect files with uninsneveruninstall flag
        std::string no_uninst;
        for (const auto& f : s->payload->files) {
            if (f.flags.find("uninsneveruninstall") != std::string::npos) {
                no_uninst += f.rel_path + "\n";
            }
        }
        if (urc > 0 || udc > 0 || !no_uninst.empty() || !installed_services.empty()
            || !reg_unins_lines.empty() || !ini_unins_lines.empty()) {
            extra += "[UninstallRun]\n";
            for (int i = 0; i < urc; ++i) {
                extra += s->payload->get("UninsRun" + std::to_string(i), "");
                extra += "\n";
            }
            extra += "[UninstallDelete]\n";
            for (int i = 0; i < udc; ++i) {
                extra += s->payload->get("UninsDel" + std::to_string(i), "");
                extra += "\n";
            }
            if (!no_uninst.empty()) {
                extra += "[NoUninstall]\n";
                extra += no_uninst;
            }
            // v0.23: services to delete on uninstall.
            // Format: one service key name per line (UTF-8).
            if (!installed_services.empty()) {
                extra += "[Services]\n";
                for (const auto& wn : installed_services) {
                    std::string n8(wn.begin(), wn.end()); // narrow (ASCII service names)
                    extra += n8;
                    extra += "\n";
                }
            }
            // v0.23: registry entries to delete on uninstall.
            // Format: root|subkey|valuetype|valuename|valuedata|uninsdeletekey
            if (!reg_unins_lines.empty()) {
                extra += "[Registry]\n";
                extra += reg_unins_lines;
            }
            // v0.24: INI entries to delete on uninstall.
            // Format: filename|section|key|value|uninsdeleteentry(1/0)
            if (!ini_unins_lines.empty()) {
                extra += "[INI]\n";
                extra += ini_unins_lines;
            }
            // write to install_dir\unins000_extra.ini
            std::wstring path = dir + L"\\unins000_extra.ini";
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(h, extra.c_str(), (DWORD)extra.size(), &written, nullptr);
                CloseHandle(h);
            }
        }
    }

    std::wstring self_dest = dir + L"\\unins000.exe";

    std::wstring reg_key  = utf8_to_wide(s->payload->get("RegKeyName", "ApTgeuApp"));
    std::wstring disp     = utf8_to_wide(s->payload->get("UninstallDisplayName",
                                                        s->payload->get("AppName", "App")));
    std::wstring ver      = utf8_to_wide(s->payload->get("AppVersion", ""));
    std::wstring pub      = utf8_to_wide(s->payload->get("Publisher", ""));
    // v0.2.5: forward the active UI language to the standalone uninstaller
    // so Control-Panel-launched unins000.exe shows the same language.
    std::wstring unins    = L"\"" + self_dest + L"\" /UNINSTALL /LANG=" +
                            utf8_to_wide(atu::i18n_current_lang());
    std::wstring disp_icon = utf8_to_wide(s->payload->get("UninstallDisplayIcon", ""));

    if (!write_uninstall_registry(reg_key, disp, ver, pub, dir, unins, disp_icon)) {
        err = "写注册表卸载项失败";
        return false;
    }

    // P1.5: Launch the app if requested (GUI mode only).
    if (s->run_after_install) {
        std::wstring exe_path = dir + L"\\" + exe_rel;
        ::ShellExecuteW(nullptr, L"open", exe_path.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    }

    // v0.24: run [Run] entries that do NOT carry postinstall (always-on).
    // Postinstall entries are deferred to the Finish page.
    run_install_entries(s, false);
    return true;
}

static void page_progress(WizardState* s) {
    ImGui::SeparatorText(atu::i18n("progress_title").c_str());
    ImGui::NewLine();

    // v0.20: resolve the chosen component set right before install starts.
    resolve_active_components(s);
    // v0.24: resolve the checked task set (gates per-task files + shortcuts).
    resolve_active_tasks(s);

    const auto& files = s->payload->files;
    int total = (int)files.size();

    if (!s->install_done && !s->install_failed) {
        // Install one file per frame so the UI stays alive.
        if (s->install_index < total) {
            const auto& f = files[s->install_index];
            s->current_file = f.rel_path;

            std::wstring dir = utf8_to_wide(s->install_dir_buf);
            std::string err;
            std::vector<std::wstring> one;
            // Install just this one file by crafting a one-element payload.
            ParsedPayload tmp;
            tmp.files.push_back(f);
            if (!install_files(tmp, dir, one, err,
                               s->active_components.empty() ? nullptr
                                                            : &s->active_components,
                               &s->active_tasks)) {
                s->install_failed = true;
                s->error_msg = err;
            } else {
                for (auto& p : one) s->written_paths.push_back(p);
                s->install_index++;
                s->progress = (float)s->install_index / (float)(total > 0 ? total : 1);
            }
        } else {
            // All files written; finalize.
            std::string err;
            if (!finalize_install(s, err)) {
                s->install_failed = true;
                s->error_msg = err;
            } else {
                s->install_done = true;
                s->progress = 1.0f;
            }
        }
    }

    ImGui::ProgressBar(s->progress, ImVec2(-FLT_MIN, 0));
    ImGui::Text("%d/%d", s->install_index, total);
    if (!s->current_file.empty()) {
        ImGui::TextWrapped("正在解压: %s", s->current_file.c_str());
    }

    if (s->install_failed) {
        ImGui::NewLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "安装失败:");
        ImGui::TextWrapped("%s", s->error_msg.c_str());
    }

    ImGui::NewLine();
    ImGui::NewLine();
    ImGui::NewLine();

    bool can_next = s->install_done;
    // Footer row: disable < 上一步 on progress.
    float button_w = 100.0f;
    float spacing  = 8.0f;
    float total_w  = button_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_w + ImGui::GetStyle().WindowPadding.x);

    if (ImGui::Button("取消", ImVec2(button_w, 0))) s->request_exit = true;
    ImGui::SameLine();
    if (!can_next) ImGui::BeginDisabled();
    if (ImGui::Button("完成 >", ImVec2(button_w, 0)) && can_next) {
        // v0.20: DisableFinishedPage -> exit immediately after install.
        if (cfg_flag_is_on(s, "DisableFinishedPage")) s->request_exit = true;
        else                                          s->page = WizardPage::Finish;
    }
    if (!can_next) ImGui::EndDisabled();
}

static void page_finish(WizardState* s) {
    ImGui::SeparatorText(atu::i18n("finish_title").c_str());
    ImGui::NewLine();
    std::string app = s->payload->get("AppName", "App");
    ImGui::TextWrapped(atu::i18n("finish_desc").c_str());
    ImGui::NewLine();

    // v0.24: per-entry "Run xxx" checkboxes for [Run] entries with postinstall.
    ensure_postinstall_checkboxes(s);
    for (size_t i = 0; i < s->postinstall_labels.size(); ++i) {
        bool checked = s->postinstall_checked[i] != 0;
        if (ImGui::Checkbox(s->postinstall_labels[i].c_str(), &checked)) {
            s->postinstall_checked[i] = checked ? 1 : 0;
        }
    }

    // Legacy global "run application" checkbox (launch <AppName>.exe).
    ImGui::Checkbox(atu::i18n("finish_run_after").c_str(), &s->run_after_install);

    ImGui::NewLine();
    // v0.20: language selector (10 languages).
    static int lang_idx = -1;
    if (lang_idx < 0) {
        // Default to the language baked into the payload config.
        const char* codes[] = {"zh","en","ja","ko","ru","de","fr","es","it","pt-BR"};
        std::string cur = s->payload->get("Language", "zh");
        lang_idx = 0;
        for (int i = 0; i < 10; ++i) if (cur == codes[i]) { lang_idx = i; break; }
    }
    ImGui::Text("Language / 语言:");
    if (ImGui::Combo("##lang", &lang_idx,
                     "Chinese\0English\0日本語\0한국어\0Русский\0Deutsch\0Français\0Español\0Italiano\0Português (Brasil)\0")) {
        const char* langs[] = {"zh", "en", "ja", "ko", "ru", "de", "fr", "es", "it", "pt-BR"};
        atu::i18n_set_language(langs[lang_idx]);
    }

    ImGui::NewLine();
    float button_w = 100.0f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - button_w + ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button(atu::i18n("btn_finish").c_str(), ImVec2(button_w, 0))) {
        // v0.24: launch ticked postinstall entries before closing.
        run_install_entries(s, true);
        s->request_exit = true;
    }
}

// ---------------------------------------------------------------------------
// Render entry
// ---------------------------------------------------------------------------

void wizard_render(WizardState* s) {
    begin_page_window(s, "ApTgeu Setup");
    ensure_components_loaded(s);   // v0.20: populate before page routing decisions
    switch (s->page) {
        case WizardPage::Welcome:  page_welcome(s);  break;
        case WizardPage::License:  page_license(s);  break;
        case WizardPage::Readme:   page_readme(s);   break;
        case WizardPage::Dir:      page_dir(s);      break;
        case WizardPage::Components: page_components(s); break;
        case WizardPage::Ready:    page_ready(s);    break;
        case WizardPage::Preparing: page_preparing(s); break;
        case WizardPage::Progress: page_progress(s); break;
        case WizardPage::Finish:   page_finish(s);   break;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Silent install (no UI)
// ---------------------------------------------------------------------------

bool wizard_run_silent_install(const ParsedPayload& p,
                               const std::wstring& install_dir,
                               std::string& err,
                               const SilentInstallOpts& opts) {
    std::wstring app_name = utf8_to_wide(p.get("AppName", "App"));

    // v0.2.5: silent-mode disk space check. If free space < required
    // (sum of all file sizes * 1.10), we do NOT abort - the task says
    // "静默模式写日志继续". Just log a WARN line and press on; the
    // caller will surface real write failures via install_files().
    {
        double required_mb = compute_required_mb(p);
        ULARGE_INTEGER free_bytes;
        if (get_free_bytes(install_dir, &free_bytes)) {
            double free_mb = (double)free_bytes.QuadPart / (1024.0 * 1024.0);
            char line[256];
            snprintf(line, sizeof(line),
                     "disk space: required=%.1f MB free=%.1f MB", required_mb, free_mb);
            atu::log_info(line);
            if (free_mb < required_mb) {
                char warn[256];
                snprintf(warn, sizeof(warn),
                         "disk space LOW (required=%.1f MB, free=%.1f MB) - continuing anyway",
                         required_mb, free_mb);
                atu::log_warn(warn);
            }
        } else {
            atu::log_warn("disk space check: GetDiskFreeSpaceExW failed, skipped");
        }
    }

    // v0.21: /FORCECLOSEAPPLICATIONS / /CLOSEAPPLICATIONS - kill the running
    // target process headlessly before overwriting files (no prompt).
    if (opts.force_close_apps || opts.close_apps) {
        std::wstring target = app_name + L".exe";
        atu::log_info(std::string("install: closing running app ") +
                      std::string(target.begin(), target.end()) +
                      (opts.force_close_apps ? " (FORCECLOSE)" : " (CLOSE)"));
        kill_process_by_exe_name(target);
    }

    // v0.24: in silent mode there is no UI, so the checked-task set is built
    // from the parser defaults (TaskEntry::checked already reflects whether
    // the script marked the task with Flags: unchecked).
    std::set<std::string> active_tasks;
    for (const auto& t : p.tasks) {
        if (t.checked) active_tasks.insert(t.name);
    }
    atu::log_info(std::string("install(silent): active_tasks=") +
                  (active_tasks.empty() ? std::string("(none)")
                                        : [&]{
                                              std::string s;
                                              for (const auto& n : active_tasks) {
                                                  if (!s.empty()) s += ",";
                                                  s += n;
                                              }
                                              return s;
                                          }()));

    std::vector<std::wstring> written;
    if (!install_files(p, install_dir, written, err,
                       nullptr, &active_tasks)) return false;

    // Create shortcuts and add to cleanup list.
    // v0.21: /NOICONS skips; /GROUP=... names the Start Menu folder.
    // v0.24: gate each shortcut on its built-in task (default on when the
    // script does not declare the task).
    if (!opts.no_icons) {
        bool want_desktop  = task_enabled(p, active_tasks, "CreateDesktopIcon");
        bool want_progmenu = task_enabled(p, active_tasks, "CreateProgramMenuIcon");
        bool want_quick    = task_enabled(p, active_tasks, "CreateQuickLaunchIcon");
        atu::log_info(std::string("install(silent): shortcut tasks desktop=") +
                      (want_desktop?"1":"0") + " progmenu=" + (want_progmenu?"1":"0") +
                      " quicklaunch=" + (want_quick?"1":"0"));
        auto shortcuts = create_shortcuts(install_dir, app_name, app_name + L".exe",
                                          opts.group_name,
                                          want_desktop, want_progmenu, want_quick);
        for (auto& s : shortcuts) written.push_back(s);
    } else {
        atu::log_info("install: /NOICONS set, skipping shortcut creation");
    }

    if (!write_unins_dat(install_dir, written)) { err = "write unins000.dat failed"; return false; }

    // Write standalone uninstaller PE.
    if (!p.unins_exe_data.empty()) {
        if (!install_unins_exe(p.unins_exe_data, install_dir)) {
            err = "write unins000.exe failed"; return false;
        }
    } else {
        std::wstring self_dest = install_dir + L"\\unins000.exe";
        if (!copy_self_to(self_dest)) { err = "copy self to unins000.exe failed"; return false; }
    }

    // v0.12: Write uninstall theme ini.
    if (!p.unins_ini_data.empty()) {
        install_unins_ini(p.unins_ini_data, install_dir);
    }

    // v0.23: apply [Registry] entries and write unins000_extra.ini so the
    // standalone uninstaller knows what to delete.
    // Also create [InstallService] services and record them for uninstall.
    {
        std::string reg_lines;
        if (!apply_registry_entries(p, install_dir, reg_lines)) {
            atu::log_warn("install(silent): one or more [Registry] entries failed");
        }
        // v0.24: apply [INI] entries (WritePrivateProfileStringW).
        std::string ini_lines;
        if (!apply_ini_entries(p, install_dir, ini_lines)) {
            atu::log_warn("install(silent): one or more [INI] entries failed");
        }
        // v0.23: create services.
        std::string install_dir_utf8 = wide_to_utf8(install_dir);
        std::vector<std::wstring> created_svcs =
            install_services_from_payload(p, install_dir_utf8);

        // v0.24: include [UninstallRun] / [UninstallDelete] / [NoUninstall] in
        // addition to registry/services/ini. The condition must match the GUI
        // path in finalize_install() so silent and GUI installs produce the
        // same unins000_extra.ini.
        int urc = 0, udc = 0;
        try { urc = std::stoi(p.get("UninsRunCount", "0")); } catch (...) {}
        try { udc = std::stoi(p.get("UninsDelCount", "0")); } catch (...) {}
        std::string no_uninst;
        for (const auto& f : p.files) {
            if (f.flags.find("uninsneveruninstall") != std::string::npos) {
                no_uninst += f.rel_path + "\n";
            }
        }
        atu::log_info("extra.ini (silent): urc=" + std::to_string(urc) +
                      " udc=" + std::to_string(udc));
        if (urc > 0 || udc > 0 || !reg_lines.empty() || !created_svcs.empty()
            || !ini_lines.empty() || !no_uninst.empty()) {
            std::string extra;
            if (urc > 0) {
                extra += "[UninstallRun]\n";
                for (int i = 0; i < urc; ++i) {
                    extra += p.get("UninsRun" + std::to_string(i), "");
                    extra += "\n";
                }
            }
            if (udc > 0) {
                extra += "[UninstallDelete]\n";
                for (int i = 0; i < udc; ++i) {
                    extra += p.get("UninsDel" + std::to_string(i), "");
                    extra += "\n";
                }
            }
            if (!no_uninst.empty()) {
                extra += "[NoUninstall]\n";
                extra += no_uninst;
            }
            if (!created_svcs.empty()) {
                extra += "[Services]\n";
                for (const auto& wn : created_svcs) {
                    std::string n8(wn.begin(), wn.end());
                    extra += n8;
                    extra += "\n";
                }
            }
            if (!ini_lines.empty()) {
                extra += "[INI]\n";
                extra += ini_lines;
            }
            if (!reg_lines.empty()) {
                extra += "[Registry]\n";
                extra += reg_lines;
            }
            std::wstring path = install_dir + L"\\unins000_extra.ini";
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                ::WriteFile(h, extra.c_str(), (DWORD)extra.size(), &w, nullptr);
                ::CloseHandle(h);
            }
        }
    }

    std::wstring self_dest = install_dir + L"\\unins000.exe";

    std::wstring reg_key = utf8_to_wide(p.get("RegKeyName", "ApTgeuApp"));
    std::wstring disp    = utf8_to_wide(p.get("UninstallDisplayName", p.get("AppName", "App")));
    std::wstring ver     = utf8_to_wide(p.get("AppVersion", ""));
    std::wstring pub     = utf8_to_wide(p.get("Publisher", ""));
    // v0.2.5: forward the active UI language to the standalone uninstaller.
    std::wstring unins   = L"\"" + self_dest + L"\" /UNINSTALL /LANG=" +
                           utf8_to_wide(atu::i18n_current_lang());
    std::wstring disp_icon = utf8_to_wide(p.get("UninstallDisplayIcon", ""));
    if (!write_uninstall_registry(reg_key, disp, ver, pub, install_dir, unins, disp_icon)) {
        err = "write uninstall registry failed";
        return false;
    }
    // v0.13 B4: write URLs to registry.
    {
        HKEY hKey;
        std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + reg_key;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            std::wstring url;
            url = utf8_to_wide(p.get("AppPublisherURL", ""));
            if (!url.empty()) ::RegSetValueExW(hKey, L"URLInfoAbout", 0, REG_SZ, (const BYTE*)url.c_str(), (DWORD)((url.size()+1)*sizeof(wchar_t)));
            url = utf8_to_wide(p.get("AppSupportURL", ""));
            if (!url.empty()) ::RegSetValueExW(hKey, L"HelpLink", 0, REG_SZ, (const BYTE*)url.c_str(), (DWORD)((url.size()+1)*sizeof(wchar_t)));
            ::RegCloseKey(hKey);
        }
    }

    // v0.24: run [Run] entries that do NOT carry postinstall (always-on).
    // Postinstall entries are skipped in silent mode (no Finish page to tick).
    {
        std::string dir_utf8 = wide_to_utf8(install_dir);
        auto entries = load_install_run_entries(p);
        atu::log_info("[Run] silent: loaded " + std::to_string(entries.size()) + " install-run entries");
        for (const auto& e : entries) {
            if (e.has_postinstall) continue;
            execute_install_run_entry(e, dir_utf8);
        }
    }
    return true;
}

} // namespace stub
