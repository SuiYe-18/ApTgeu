/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu stub - Dear ImGui wizard pages.
//
// A single WizardState struct carries all cross-page state. main.cpp owns
// the GL context and message loop; gui_pages renders one page per frame.

#pragma once

#include <string>
#include <vector>
#include <set>
#include "unpack.h"

// Forward declare GLuint without pulling <GL/gl.h> here.
typedef unsigned int GLuint;

namespace stub {

enum class WizardPage {
    Welcome = 0,
    License,
    Readme,       // v0.19: shown if ReadmeFile exists
    Dir,
    Components,
    Ready,        // v0.19: install summary
    Preparing,    // v0.19: kill running app
    Progress,
    Finish,
};

struct WizardState {
    // In (owned by main.cpp, borrowed here)
    const ParsedPayload* payload = nullptr;

    // User input / flow
    WizardPage page = WizardPage::Welcome;
    std::string readme_text;  // v0.19: readme content
    char        install_dir_buf[4096] = {0};   // ImGui InputText edits this (UTF-8)
    bool        agree_license = false;
    bool        request_exit  = false;
    bool        run_after_install = true;   // finish page checkbox (legacy: launch <AppName>.exe)

    // v0.24: per-entry state for [Run] entries that carry the postinstall flag.
    // One bool per such entry, default true. Populated lazily on the Finish page.
    std::vector<char> postinstall_checked;       // vector<char> so ImGui::Checkbox can take &v[0]
    std::vector<std::string> postinstall_labels; // UTF-8 label for each checkbox
    bool postinstall_state_loaded = false;

    // v0.4: component selection.
    // available_components = list of (name, description) parsed from config.
    // selected = parallel bool vector, one per component (default true).
    std::vector<std::pair<std::string,std::string>> available_components;
    std::vector<char> component_selected;  // vector<bool> 不能取地址给 ImGui::Checkbox

    // v0.20: install type radios on the components page.
    //   0 = typical  (推荐预设: first component required; rest pre-checked)
    //   1 = complete (勾所有组件)
    //   2 = custom   (用户手动勾选)
    int  install_type      = 0;
    bool components_loaded = false;   // lazily populate from payload.config
    std::set<std::string> active_components;  // resolved when leaving Components page
    std::set<std::string> active_tasks;       // v0.24: resolved from payload->tasks[i].checked

    // Install progress (driven one-file-per-frame on the Progress page)
    int   install_index = 0;
    float progress      = 0.0f;
    std::string current_file;
    bool  install_done  = false;
    bool  install_failed = false;
    std::string error_msg;
    std::vector<std::wstring> written_paths;

    // Welcome image as GL texture (0 = none). Backwards-compat: if the
    // UI block has no welcome_banner but the legacy APTGEU::welcome.png
    // entry exists, we load that into welcome_tex instead.
    GLuint welcome_tex = 0;
    int    welcome_w   = 0;
    int    welcome_h   = 0;

    // Extra theme textures from UI block. 0 = not present.
    GLuint sidebar_tex    = 0; int sidebar_w    = 0; int sidebar_h    = 0;
    GLuint background_tex = 0; int background_w = 0; int background_h = 0;
    GLuint footer_tex     = 0; int footer_w     = 0; int footer_h     = 0;

    // v0.21: CLI flags propagated from main.cpp (/NOICONS, /GROUP=...).
    bool        no_icons   = false;
    std::wstring group_name;
};

// Decode every PNG in payload->theme (and the legacy welcome_png fallback)
// into OpenGL textures. Safe to call when payload->theme.loaded == false.
void wizard_init_theme_textures(WizardState* s);

// v0.20: read [Setup] Disable*Page / Wizard* flags from payload->config and
// adjust the initial page. At minimum honors DisableWelcomePage (skips the
// welcome page and jumps to the next enabled page). Call once after
// state.payload is set, before the first frame.
void wizard_apply_page_flags(WizardState* s);

// Push ImGui style colors / vars from the theme. Call once after ImGui
// context creation and before NewFrame. On the next frame you must NOT
// pop these (they live for the whole process); this matches the typical
// "style once" pattern.
void wizard_apply_theme(const WizardState* s);

// Render one ImGui frame for the current page. Call between ImGui::NewFrame()
// and ImGui::Render(). May advance state->page and drive install progress.
void wizard_render(WizardState* s);

// Run the full install synchronously (used by /SILENT mode; no UI).
// Returns true on success; fills err on failure.
struct SilentInstallOpts {
    bool        no_icons        = false;  // /NOICONS
    bool        force_close_apps = false; // /FORCECLOSEAPPLICATIONS
    bool        close_apps      = false;  // /CLOSEAPPLICATIONS
    std::wstring group_name;              // /GROUP=...
};
bool wizard_run_silent_install(const ParsedPayload& p,
                               const std::wstring& install_dir,
                               std::string& err,
                               const SilentInstallOpts& opts = SilentInstallOpts{});

} // namespace stub
