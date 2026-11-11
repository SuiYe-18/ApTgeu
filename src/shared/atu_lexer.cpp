/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
#include "atu_lexer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cstdio>

namespace atu {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) --e;
    return s.substr(b, e - b);
}

// Read a "quoted string" starting at index i, returns content, advances end_pos past closing quote.
// If no quote at i, read until whitespace or ';' or end.
std::string read_value(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) return "";
    if (s[i] == '"') {
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            out.push_back(s[i]);
            ++i;
        }
        if (i < s.size()) ++i; // skip closing quote
        return out;
    }
    // unquoted: read until ; or whitespace
    std::string out;
    while (i < s.size() && s[i] != ';' && !std::isspace(static_cast<unsigned char>(s[i]))) {
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Parse a [Files] line like:  Source: "D:\x\y.exe"; DestDir: "{app}"; Flags: ignoreversion
FileEntry parse_files_line(const std::string& line) {
    FileEntry fe;
    // split on ;
    std::string cur;
    std::vector<std::string> parts;
    for (char c : line) {
        if (c == ';') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);

    for (auto& p : parts) {
        std::string t = trim(p);
        if (t.empty()) continue;
        // find ':'
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(t.substr(0, colon));
        std::string val_part = t.substr(colon + 1);
        size_t i = 0;
        std::string val = read_value(val_part, i);
        if (key == "Source") fe.source = val;
        else if (key == "DestDir") fe.destdir = val;
        else if (key == "Flags") fe.flags = val;
        else if (key == "Components") fe.component = val;
        else if (key == "Tasks") fe.tasks = val;
    }
    if (fe.source.empty()) {
        // External download lines may have Source as URL in some cases,
        // but if we still can't find Source, skip silently (e.g. ExternalSize-only lines).
        // Only throw for non-external, non-ignored lines.
        if (fe.flags.find("external") == std::string::npos &&
            fe.flags.find("isreadme") == std::string::npos) {
            throw std::runtime_error("[Files] line missing Source: " + line);
        }
    }
    return fe;
}

} // namespace

AtuScript parse_atu_text(const std::string& text) {
    AtuScript scr;
    std::istringstream in(text);
    std::string line;
    std::string section; // "Setup" or "Files"
    std::string cont; // line continuation buffer (Inno uses trailing backslash)
    while (std::getline(in, line)) {
        // strip CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty()) continue;
        // Skip pure comment lines, but don't break continuation mid-block
        if (t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[' && t.back() == ']') {
            // New section ends any continuation
            cont.clear();
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        // Line continuation: trailing backslash means join with next line
        if (!t.empty() && t.back() == '\\') {
            cont += t.substr(0, t.size() - 1);
            continue;
        }
        if (!cont.empty()) {
            t = cont + t;
            cont.clear();
        }
        if (section == "Setup") {
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(t.substr(0, eq));
            std::string v = trim(t.substr(eq + 1));
            scr.setup[k] = v;
        } else if (section == "Files") {
            scr.files.push_back(parse_files_line(t));
        } else if (section == "Components") {
            // Lines: Name=main; Description=主程序
            ComponentEntry ce;
            std::string cur;
            std::vector<std::string> parts;
            for (char c : t) {
                if (c == ';') { parts.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p);
                if (pt.empty()) continue;
                size_t eq = pt.find('=');
                if (eq == std::string::npos) continue;
                std::string k = trim(pt.substr(0, eq));
                std::string v = trim(pt.substr(eq + 1));
                if (k == "Name") ce.name = v;
                else if (k == "Description") ce.description = v;
            }
            if (!ce.name.empty()) scr.components.push_back(std::move(ce));
        } else if (section == "Run" || section == "UninstallRun" || section == "InstallRun") {
            // v0.24: split [Run] (install-time, not shipped to uninstaller) from
            // [UninstallRun] (shipped to unins000_extra.ini). [InstallRun] is a
            // legacy alias kept for compatibility.
            RunEntry re;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                // strip surrounding quotes (Inno-style: Filename="..." or Filename: "...")
                if (v.size() >= 2 && v.front()=='"' && v.back()=='"') v = v.substr(1, v.size()-2);
                if (k=="Filename") re.filename=v;
                else if (k=="Parameters") re.parameters=v;
                else if (k=="WorkingDir") re.workingdir=v;
                else if (k=="Description") re.description=v;
                else if (k=="Flags") re.flags=v;
            }
            if (re.filename.empty()) continue;
            if (section == "UninstallRun") scr.unins_run.push_back(std::move(re));
            else if (section == "InstallRun") scr.install_run.push_back(std::move(re));
            else scr.run_entries.push_back(std::move(re));
        } else if (section == "Tasks") {
            TaskEntry te;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Name") te.name=v;
                else if (k=="Description") te.description=v;
                else if (k=="Flags" && v.find("unchecked")!=std::string::npos) te.unchecked=true;
            }
            if (!te.name.empty()) scr.tasks.push_back(std::move(te));
        } else if (section == "Registry") {
            RegEntry re;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Root") re.root=v;
                else if (k=="Subkey") re.subkey=v;
                else if (k=="ValueType") re.valuetype=v;
                else if (k=="ValueName") re.valuename=v;
                else if (k=="ValueData") re.valuedata=v;
                else if (k=="Flags" && v.find("uninsdeletekey")!=std::string::npos) re.uninsdeletekey=true;
            }
            if (!re.subkey.empty()) scr.reg_entries.push_back(std::move(re));
        } else if (section == "INI") {
            IniEntry ie;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                // strip surrounding double quotes from value
                if (v.size() >= 2 && v.front()=='"' && v.back()=='"') v = v.substr(1, v.size()-2);
                if (k=="Filename") ie.filename=v;
                else if (k=="Section") ie.section=v;
                else if (k=="Key") ie.key=v;
                else if (k=="String") ie.value=v;
                else if (k=="Flags") ie.flags=v;
            }
            if (!ie.filename.empty()) scr.ini_entries.push_back(std::move(ie));
        } else if (section == "InstallDelete" || section == "UninstallDelete") {
            DelEntry de;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Type") de.type=v;
                else if (k=="Name") de.name=v;
            }
            if (!de.name.empty()) {
                if (section == "InstallDelete") scr.install_delete.push_back(std::move(de));
                else scr.unins_delete.push_back(std::move(de));
            }
        } else if (section == "Icons") {
            IconEntry ie;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Name") ie.name=v;
                else if (k=="Filename") ie.filename=v;
                else if (k=="IconFilename") ie.icon_filename=v;
                else if (k=="Comment") ie.comment=v;
                else if (k=="Parameters") ie.parameters=v;
                else if (k=="WorkingDir") ie.workingdir=v;
            }
            if (!ie.name.empty()) scr.icons.push_back(std::move(ie));
        } else if (section == "Types") {
            TypeEntry te;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Name") te.name=v;
                else if (k=="Description") te.description=v;
            }
            if (!te.name.empty()) scr.types.push_back(std::move(te));
        } else if (section == "Dirs") {
            DirEntry de;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Name") de.name=v;
                else if (k=="Flags") de.flags=v;
            }
            if (!de.name.empty()) scr.dirs.push_back(std::move(de));
        } else if (section == "InstallService") {
            // v0.23: Name=...; DisplayName=...; Description=...; StartType=...; Executable=...
            ServiceEntry se;
            std::string cur; std::vector<std::string> parts;
            for (char c : t) { if (c==';'){parts.push_back(cur);cur.clear();} else cur.push_back(c); }
            parts.push_back(cur);
            for (auto& p : parts) {
                std::string pt = trim(p); if (pt.empty()) continue;
                size_t eq = pt.find('='); if (eq==std::string::npos) continue;
                std::string k=trim(pt.substr(0,eq)), v=trim(pt.substr(eq+1));
                if (k=="Name") se.name=v;
                else if (k=="DisplayName") se.display_name=v;
                else if (k=="Description") se.description=v;
                else if (k=="StartType") se.start_type=v;
                else if (k=="Executable") se.executable=v;
            }
            if (!se.name.empty() && !se.executable.empty()) scr.services.push_back(std::move(se));
        }
        // else: unknown section, ignore
    }
    return scr;
}

AtuScript parse_atu_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open .atu file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_atu_text(ss.str());
}

} // namespace atu
