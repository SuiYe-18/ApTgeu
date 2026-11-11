/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "payload_format.h"
#include "crc32.h"

namespace atu {

void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void write_i32_le(std::vector<uint8_t>& out, int32_t v) {
    write_u32_le(out, static_cast<uint32_t>(v));
}

void write_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
}

void write_bytes(std::vector<uint8_t>& out, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

void write_bytes(std::vector<uint8_t>& out, const std::string& s) {
    write_bytes(out, s.data(), s.size());
}

void write_bytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
    out.insert(out.end(), v.begin(), v.end());
}

namespace {
// Append a 0xRRGGBB color as 4 bytes RGBA (R,G,B,255).
void write_color_rgba(std::vector<uint8_t>& out, uint32_t rgb) {
    out.push_back(static_cast<uint8_t>((rgb >> 16) & 0xFF)); // R
    out.push_back(static_cast<uint8_t>((rgb >>  8) & 0xFF)); // G
    out.push_back(static_cast<uint8_t>( rgb        & 0xFF)); // B
    out.push_back(0xFF);                                     // A
}
} // namespace

std::vector<uint8_t> build_footer(uint64_t payload_start_offset, const char magic[8]) {
    std::vector<uint8_t> out;
    write_bytes(out, magic, 8);
    write_u64_le(out, payload_start_offset);
    return out;
}

// v0.16: extended footer with slicing support
std::vector<uint8_t> build_footer_v16(uint64_t payload_start_offset,
                                       uint64_t total_payload_size,
                                       uint32_t current_slice) {
    std::vector<uint8_t> out;
    const char magic[8] = {'A','P','T','G','E','U','0','6'};
    write_bytes(out, magic, 8);
    write_u64_le(out, payload_start_offset);
    write_u64_le(out, total_payload_size);
    write_u32_le(out, current_slice);
    return out;
}

std::vector<uint8_t> build_footer(uint64_t payload_start_offset) {
    const char magic[8] = {'A','P','T','G','E','U','0','3'};
    return build_footer(payload_start_offset, magic);
}

void SerializeUiBlock(const AtuiTheme& t, std::vector<uint8_t>& out) {
    write_i32_le(out, static_cast<int32_t>(t.window_width));
    write_i32_le(out, static_cast<int32_t>(t.window_height));
    write_i32_le(out, t.resizable ? 1 : 0);

    std::string title_utf8 = AtuiTitleToUtf8(t);
    write_u32_le(out, static_cast<uint32_t>(title_utf8.size()));
    write_bytes(out, title_utf8);

    // 9 colors, RGBA
    write_color_rgba(out, t.primary);
    write_color_rgba(out, t.primary_hover);
    write_color_rgba(out, t.background);
    write_color_rgba(out, t.panel);
    write_color_rgba(out, t.text);
    write_color_rgba(out, t.text_dim);
    write_color_rgba(out, t.button);
    write_color_rgba(out, t.button_text);
    write_color_rgba(out, t.border);

    // 7 layout/font int32s
    write_i32_le(out, static_cast<int32_t>(t.sidebar_width));
    write_i32_le(out, static_cast<int32_t>(t.titlebar_height));
    write_i32_le(out, static_cast<int32_t>(t.button_height));
    write_i32_le(out, static_cast<int32_t>(t.button_width));
    write_i32_le(out, static_cast<int32_t>(t.item_spacing_y));
    write_i32_le(out, static_cast<int32_t>(t.margin));
    write_i32_le(out, static_cast<int32_t>(t.font_size));

    // 4 images, uint32 len + bytes
    write_u32_le(out, static_cast<uint32_t>(t.welcome_banner_png.size()));
    write_bytes(out, t.welcome_banner_png);
    write_u32_le(out, static_cast<uint32_t>(t.sidebar_image_png.size()));
    write_bytes(out, t.sidebar_image_png);
    write_u32_le(out, static_cast<uint32_t>(t.background_image_png.size()));
    write_bytes(out, t.background_image_png);
    write_u32_le(out, static_cast<uint32_t>(t.footer_logo_png.size()));
    write_bytes(out, t.footer_logo_png);
}

std::vector<uint8_t> build_payload(const Resolved& r,
                                   const std::vector<PayloadFile>& files,
                                   const AtuiTheme* theme,
                                   const std::vector<ComponentEntry>* components,
                                   const std::vector<TaskEntry>* tasks,
                                   bool verify_md5,
                                   const std::vector<RegEntry>* reg_entries,
                                   const std::vector<ServiceEntry>* services,
                                   const std::vector<IniEntry>* ini_entries,
                                   const std::vector<RunEntry>* unins_run,
                                   const std::vector<RunEntry>* install_run) {
    // 1. Build config_text: each line "Key=Value\n"
    std::string cfg;
    auto add_line = [&](const std::string& k, const std::string& v) {
        cfg += k; cfg += '='; cfg += v; cfg += '\n';
    };
    add_line("AppName", r.app_name);
    add_line("AppVersion", r.app_version);
    add_line("Publisher", r.publisher);
    add_line("DefaultDirName", r.default_dir);
    add_line("UninstallDisplayName", r.uninstall_display_name);
    add_line("RegKeyName", r.reg_key_name);
    add_line("Language", r.language);
    // v0.2.2: tell the stub to expect and verify per-file MD5.
    if (verify_md5) add_line("VerifyHash", "md5");
    // v0.13 B4
    if (!r.app_publisher_url.empty()) add_line("AppPublisherURL", r.app_publisher_url);
    if (!r.app_support_url.empty()) add_line("AppSupportURL", r.app_support_url);
    if (!r.updates_url.empty()) add_line("UpdatesURL", r.updates_url);
    if (!r.readme_file.empty()) add_line("ReadmeFile", r.readme_file);
    if (!r.min_version.empty()) add_line("MinVersion", r.min_version);
    if (!r.default_group_name.empty()) add_line("DefaultGroupName", r.default_group_name);

    // v0.20: wizard / uninstall flags. Booleans are emitted as 1/0 so the
    // stub can read them unconditionally; strings only when non-empty.
    add_line("DisableWelcomePage",     r.disable_welcome_page     ? "1" : "0");
    add_line("DisableReadmePage",      r.disable_readme_page      ? "1" : "0");
    add_line("DisableDirPage",         r.disable_dir_page         ? "1" : "0");
    add_line("DisableComponentsPage",  r.disable_components_page  ? "1" : "0");
    add_line("DisableReadyPage",       r.disable_ready_page       ? "1" : "0");
    add_line("DisableFinishedPage",    r.disable_finished_page    ? "1" : "0");
    add_line("WizardResizable",        r.wizard_resizable         ? "1" : "0");
    add_line("AllowNoIcons",           r.allow_no_icons           ? "1" : "0");
    add_line("AllowRootDirectory",     r.allow_root_directory     ? "1" : "0");
    if (!r.wizard_style.empty())           add_line("WizardStyle",           r.wizard_style);
    if (!r.uninstall_display_icon.empty()) add_line("UninstallDisplayIcon", r.uninstall_display_icon);
    // v0.21: registry of Pascal [Code] procedure/function names (informational;
    // we never execute Pascal, but the list is stored so tooling / future
    // versions know which entry points a script declared).
    if (!r.code_funcs.empty()) add_line("CodeFuncs", r.code_funcs);

    // v0.22: VC++ 2015-2022 x64 redistributable auto-install flag.
    // The stub reads this to decide whether to query the registry and run
    // the embedded bootstrapper.
    if (r.install_vcredist) add_line("InstallVcRedist", "1");
    // v0.23: bootstrapper download URL. The stub uses this only when no
    // local VcRedistSource file was embedded into the payload.
    if (!r.vcredist_url.empty()) add_line("VcRedistURL", r.vcredist_url);
    // v0.23: test/debug hook: force bootstrapper run regardless of detection.
    if (!r.vcredist_force.empty()) add_line("VcRedistForceInstall", r.vcredist_force);

    // v0.4: components
    if (components && !components->empty()) {
        std::string names, descs;
        for (size_t i = 0; i < components->size(); ++i) {
            if (i) { names += ","; descs += "|"; }
            names += (*components)[i].name;
            descs += (*components)[i].name;
            descs += ":";
            descs += (*components)[i].description;
        }
        add_line("Components", names);
        add_line("ComponentsDesc", descs);
    }
    // v0.14: Tasks
    if (tasks && !tasks->empty()) {
        std::string tn, td, tu;
        for (size_t i=0;i<tasks->size();++i) {
            if (i) { tn+=","; td+="|"; tu+=","; }
            tn += (*tasks)[i].name;
            td += (*tasks)[i].name; td += ":"; td += (*tasks)[i].description;
            tu += (*tasks)[i].unchecked ? "1" : "0";
        }
        add_line("Tasks", tn);
        add_line("TasksDesc", td);
        add_line("TasksUnchecked", tu);
    }
    // v0.14: Registry entries (serialized as RegN=root|subkey|valuetype|valuename|valuedata|uninsdeletekey)
    // The stub reads these at install time and writes them to the registry.
    // {app} in subkey/valuename/valuedata is substituted by the stub.
    if (reg_entries && !reg_entries->empty()) {
        for (size_t i = 0; i < reg_entries->size(); ++i) {
            const auto& re = (*reg_entries)[i];
            std::string val = re.root + "|" + re.subkey + "|" + re.valuetype + "|"
                            + re.valuename + "|" + re.valuedata + "|"
                            + (re.uninsdeletekey ? "1" : "0");
            add_line("Reg" + std::to_string(i), val);
        }
        add_line("RegCount", std::to_string(reg_entries->size()));
    }

    // v0.23: Windows service entries.
    // Wire format: SvcN = name|display_name|description|start_type|executable
    // The stub calls OpenSCManagerW + CreateServiceW at install time and
    // OpenServiceW + DeleteServiceW at uninstall time.
    // {app} in Executable is substituted by the stub at runtime.
    if (services && !services->empty()) {
        for (size_t i = 0; i < services->size(); ++i) {
            const auto& se = (*services)[i];
            std::string val = se.name + "|" + se.display_name + "|" + se.description
                            + "|" + se.start_type + "|" + se.executable;
            add_line("Svc" + std::to_string(i), val);
        }
        add_line("SvcCount", std::to_string(services->size()));
    }

    // v0.24: [INI] entries.
    // Wire format: IniN = filename|section|key|value|flags
    // The stub calls WritePrivateProfileStringW at install time. On uninstall,
    // if flags contains "uninsdeleteentry", the stub calls
    // WritePrivateProfileStringW with a NULL value to delete the entry.
    // {app} in filename is substituted by the stub at runtime.
    if (ini_entries && !ini_entries->empty()) {
        for (size_t i = 0; i < ini_entries->size(); ++i) {
            const auto& ie = (*ini_entries)[i];
            std::string val = ie.filename + "|" + ie.section + "|" + ie.key
                            + "|" + ie.value + "|" + ie.flags;
            add_line("Ini" + std::to_string(i), val);
        }
        add_line("IniCount", std::to_string(ini_entries->size()));
    }

    // v0.24: [UninstallRun] entries.
    // Wire format: UninsRunN = filename|parameters|workingdir|flags
    // The stub copies these verbatim into unins000_extra.ini's [UninstallRun]
    // section; the standalone uninstaller substitutes {app} and launches them
    // BEFORE deleting files (Inno order).
    if (unins_run && !unins_run->empty()) {
        for (size_t i = 0; i < unins_run->size(); ++i) {
            const auto& re = (*unins_run)[i];
            std::string val = re.filename + "|" + re.parameters + "|"
                            + re.workingdir + "|" + re.flags;
            add_line("UninsRun" + std::to_string(i), val);
        }
        add_line("UninsRunCount", std::to_string(unins_run->size()));
    }

    // v0.24: [Run] entries (install-time, after files are laid down).
    // Wire: InstRunN = filename|parameters|workingdir|flags|description
    // The stub substitutes {app} in filename/workingdir and CreateProcess's
    // the result. Entries with the `postinstall` flag only fire when the
    // Finish-page checkbox is ticked.
    if (install_run && !install_run->empty()) {
        for (size_t i = 0; i < install_run->size(); ++i) {
            const auto& re = (*install_run)[i];
            std::string val = re.filename + "|" + re.parameters + "|"
                            + re.workingdir + "|" + re.flags + "|" + re.description;
            add_line("InstRun" + std::to_string(i), val);
        }
        add_line("InstRunCount", std::to_string(install_run->size()));
    }

    std::vector<uint8_t> out;
    write_u32_le(out, static_cast<uint32_t>(cfg.size()));
    write_bytes(out, cfg);

    // 2. UI block (v0.1.1)
    std::vector<uint8_t> ui_block;
    if (theme) SerializeUiBlock(*theme, ui_block);
    write_u32_le(out, static_cast<uint32_t>(ui_block.size()));
    write_bytes(out, ui_block);

    // 3. File table
    write_u32_le(out, static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
        write_u32_le(out, static_cast<uint32_t>(f.rel_path.size()));
        write_bytes(out, f.rel_path);
        write_u32_le(out, static_cast<uint32_t>(f.component.size()));  // v0.4
        write_bytes(out, f.component);
        write_u32_le(out, static_cast<uint32_t>(f.tasks.size()));      // v0.14
        write_bytes(out, f.tasks);
        write_u32_le(out, static_cast<uint32_t>(f.flags.size()));      // v0.19
        write_bytes(out, f.flags);
        write_u32_le(out, static_cast<uint32_t>(f.data.size()));
        write_u32_le(out, crc32(f.data));
        out.push_back(f.compression);  // v0.11: 0=store, 1=lzma2
        if (verify_md5) {
            // v0.2.2: 16-byte MD5 of f.data, computed by the caller (packer).
            out.insert(out.end(), f.md5, f.md5 + 16);
        }
        write_bytes(out, f.data);
    }
    return out;
}

} // namespace atu
