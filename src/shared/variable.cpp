/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "variable.h"
#include "atu_lexer.h"

namespace atu {

namespace {

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// v0.20: parse "1/yes/true/on" -> true; "0/no/false/off" -> false; else def.
bool parse_bool(const std::string& s, bool def) {
    std::string t; t.reserve(s.size());
    for (char c : s) t.push_back((char)std::tolower((unsigned char)c));
    size_t b = t.find_first_not_of(" \t\r\n");
    size_t e = t.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return def;
    t = t.substr(b, e - b + 1);
    if (t == "1" || t == "yes" || t == "true" || t == "on")  return true;
    if (t == "0" || t == "no"  || t == "false" || t == "off") return false;
    return def;
}

// v0.21: {code:FuncName} placeholders - we don't execute Pascal, so
// they resolve to empty string. Never error on unknown code refs.
// Idempotent: safe to call from both pass1 and pass2.
std::string strip_code_refs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t k = 0;
    while (k < s.size()) {
        if (s.compare(k, 6, "{code:") == 0) {
            size_t end = s.find('}', k + 6);
            if (end == std::string::npos) { out.append(s, k, std::string::npos); break; }
            k = end + 1;
        } else {
            out += s[k++];
        }
    }
    return out;
}

std::string pass1(const std::string& raw, const Resolved& r) {
    std::string s = raw;
    s = replace_all(s, "{autopf32}", "C:\\Program Files (x86)");
    s = replace_all(s, "{autopf}", "C:\\Program Files");
    s = replace_all(s, "{#AppVersion}", r.app_version);
    s = replace_all(s, "{#AppName}", r.app_name);
    s = strip_code_refs(s);
    return s;
}

std::string pass2(const std::string& raw, const Resolved& r) {
    std::string s = raw;
    s = replace_all(s, "{app}", r.default_dir);
    s = replace_all(s, "{pf}", "C:\\Program Files");
    s = replace_all(s, "{sys}", "C:\\Windows\\System32");
    s = replace_all(s, "{win}", "C:\\Windows");
    s = replace_all(s, "{src}", "");  // source dir = compiler cwd
    s = replace_all(s, "{tmp}", "C:\\Temp");
    s = replace_all(s, "{commonfonts}", "C:\\Windows\\Fonts");
    s = replace_all(s, "{group}", "");  // start menu group, resolved at runtime
    s = strip_code_refs(s);
    return s;
}

} // namespace

std::string substitute(const std::string& raw, const Resolved& r) {
    return pass2(pass1(raw, r), r);
}

Resolved resolve_script(const AtuScript& scr) {
    Resolved r;
    r.app_name = scr.get("AppName", "MyApp");
    r.app_version = scr.get("AppVersion", "1.0.0");
    r.publisher = scr.get("Publisher", "");
    r.welcome_image = scr.get("WelcomeImage", "");
    r.license_file = scr.get("LicenseFile", "");
    r.output_dir = scr.get("OutputDir", "Output");
    r.output_base_name = scr.get("OutputBaseName", "setup");
    r.reg_key_name = scr.get("RegKeyName", "ApTgeu_" + r.app_name);
    r.language = scr.get("Language", "zh");
    r.compression = scr.get("Compression", "lzma2");
    // perf knobs: CompressionLevel=1..9, CompressionDictionary=<bytes|Nm|NM>
    try {
        std::string lvl = scr.get("CompressionLevel", "9");
        if (!lvl.empty()) r.lzma_level = std::stoi(lvl);
        if (r.lzma_level < 1) r.lzma_level = 1;
        if (r.lzma_level > 9) r.lzma_level = 9;
    } catch (...) {}
    try {
        std::string dic = scr.get("CompressionDictionary", "");
        if (!dic.empty()) {
            // accept plain bytes, or trailing K/M/G suffix
            size_t mul = 1;
            char suf = dic.back();
            if (suf=='K'||suf=='k') { mul=1024; dic.pop_back(); }
            else if (suf=='M'||suf=='m') { mul=1024*1024; dic.pop_back(); }
            else if (suf=='G'||suf=='g') { mul=1024u*1024*1024; dic.pop_back(); }
            r.lzma_dict = static_cast<uint32_t>(std::stoull(dic) * mul);
        }
    } catch (...) {}
    r.uninstall_ui_file = scr.get("UninstallUiFile", "");
    r.app_publisher_url = scr.get("AppPublisherURL", "");
    r.app_support_url = scr.get("AppSupportURL", "");
    r.updates_url = scr.get("UpdatesURL", "");
    r.readme_file = scr.get("ReadmeFile", "");
    r.min_version = scr.get("MinVersion", "");
    r.default_group_name = scr.get("DefaultGroupName", r.app_name);

    // v0.20: wizard / uninstall flags.
    r.disable_welcome_page     = parse_bool(scr.get("DisableWelcomePage", ""),     false);
    r.disable_readme_page      = parse_bool(scr.get("DisableReadmePage", ""),      false);
    r.disable_dir_page         = parse_bool(scr.get("DisableDirPage", ""),         false);
    r.disable_components_page  = parse_bool(scr.get("DisableComponentsPage", ""),  false);
    r.disable_ready_page       = parse_bool(scr.get("DisableReadyPage", ""),       false);
    r.disable_finished_page    = parse_bool(scr.get("DisableFinishedPage", ""),    false);
    r.wizard_style             = scr.get("WizardStyle", "");
    r.wizard_resizable         = parse_bool(scr.get("WizardResizable", ""),        false);
    r.uninstall_display_icon   = scr.get("UninstallDisplayIcon", "");
    r.allow_no_icons           = parse_bool(scr.get("AllowNoIcons", ""),           false);
    r.allow_root_directory     = parse_bool(scr.get("AllowRootDirectory", ""),     false);
    r.code_funcs               = scr.get("CodeFuncs", "");

    // v0.22: VC++ 2015-2022 x64 redistributable auto-install.
    r.install_vcredist         = parse_bool(scr.get("InstallVcRedist", ""), false);
    r.vcredist_source          = scr.get("VcRedistSource", "");
    // v0.23: bootstrapper download URL (used when no local source was embedded).
    r.vcredist_url             = scr.get("VcRedistURL", "");
    // v0.23: test/debug hook - skip registry detection and always run bootstrapper.
    r.vcredist_force           = scr.get("VcRedistForceInstall", "");

    // Pass 1 on DefaultDirName first to derive {app}
    std::string raw_default = scr.get("DefaultDirName", "C:\\Program Files\\" + r.app_name);
    r.default_dir = pass1(raw_default, r);

    // Then UninstallDisplayName: pass1 then pass2 ({app})
    r.uninstall_display_name = substitute(scr.get("UninstallDisplayName", r.app_name + " " + r.app_version), r);
    return r;
}

} // namespace atu
