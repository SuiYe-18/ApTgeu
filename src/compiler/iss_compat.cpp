// v0.17: .iss -> .atu compatibility layer
// Reads Inno Setup .iss text, renames fields/sections to .atu names,
// then feeds through the normal parser.
// v0.21: [Code] section is shallow-scanned for procedure/function names.
// v0.2.3: the [Code] body is also loaded into the mini Pascal interpreter
//         (src/shared/pascal.*). {code:FuncName} placeholders in [Setup]
//         values are evaluated at compile time and replaced by the string
//         the named function returns.
#include "atu_lexer.h"
#include "pascal.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace atu {

static std::string replace_all_str(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Shallow Pascal scanner: extract top-level procedure/function names.
// Strings and comments are neutralized before tokenizing so that the words
// "procedure"/"function" inside them don't produce false positives.
// We deliberately do NOT validate Pascal syntax - anything we don't
// understand is just ignored.
static std::vector<std::string> parse_pascal_code(const std::string& code) {
    std::vector<std::string> funcs;
    std::string buf;
    buf.reserve(code.size());

    auto is_id_char = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    };
    auto is_id_start = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };

    // Pass 1: neutralize string literals and comments.
    size_t i = 0, n = code.size();
    while (i < n) {
        char c = code[i];
        // Single-quoted string literal ('' = escaped quote)
        if (c == '\'') {
            buf += ' ';
            ++i;
            while (i < n && code[i] != '\'') {
                if (code[i] == '\'' && i + 1 < n && code[i+1] == '\'') {
                    buf += "  "; i += 2; continue;
                }
                buf += ' '; ++i;
            }
            if (i < n) { buf += ' '; ++i; }
            continue;
        }
        // { ... } comment (but not {$IFDEF} compiler directives - we still
        // neutralize them; they can't contain a real procedure/function decl).
        if (c == '{') {
            buf += ' '; ++i;
            while (i < n && code[i] != '}') {
                buf += (code[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            if (i < n) { buf += ' '; ++i; }
            continue;
        }
        // (* ... *) block comment
        if (c == '(' && i + 1 < n && code[i+1] == '*') {
            buf += "  "; i += 2;
            while (i + 1 < n && !(code[i] == '*' && code[i+1] == ')')) {
                buf += (code[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            if (i + 1 < n) { buf += "  "; i += 2; }
            continue;
        }
        // // line comment
        if (c == '/' && i + 1 < n && code[i+1] == '/') {
            buf += ' ';
            while (i < n && code[i] != '\n') { buf += ' '; ++i; }
            continue;
        }
        buf += c;
        ++i;
    }

    // Pass 2: tokenize, find 'procedure'/'function' keywords, grab the
    // following identifier. Deduplicate case-insensitively.
    i = 0; n = buf.size();
    while (i < n) {
        if (!is_id_char(buf[i])) { ++i; continue; }
        size_t start = i;
        while (i < n && is_id_char(buf[i])) ++i;
        std::string word = buf.substr(start, i - start);
        std::string lw; lw.reserve(word.size());
        for (char ch : word)
            lw.push_back((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch);

        if (lw == "procedure" || lw == "function") {
            // skip punctuation/whitespace until the next identifier (the name)
            size_t j = i;
            while (j < n && !is_id_start(buf[j])) ++j;
            if (j < n) {
                size_t s2 = j;
                while (j < n && is_id_char(buf[j])) ++j;
                std::string name = buf.substr(s2, j - s2);
                bool dup = false;
                for (const auto& f : funcs) {
                    if (_stricmp(f.c_str(), name.c_str()) == 0) { dup = true; break; }
                }
                if (!dup) funcs.push_back(name);
                i = j;
            } else {
                i = n;
            }
        }
    }
    return funcs;
}

AtuScript parse_iss_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    // Strip BOM
    if (text.size() >= 3 && (uint8_t)text[0]==0xEF && (uint8_t)text[1]==0xBB && (uint8_t)text[2]==0xBF)
        text = text.substr(3);

    // Field name mappings (case-insensitive in Inno, but our parser is case-sensitive for keys)
    struct { const char* from; const char* to; } maps[] = {
        {"AppVersionName", "AppVersion"},
        {"AppPublisherURL", "AppPublisherURL"},  // same
        {"SetupIconFile", "SetupIcon"},
        {"OutputBaseFilename", "OutputBaseName"},
        {"Compression=lzma2", "Compression=lzma2"},
        {"Compression=lzma", "Compression=lzma2"},
        {"Compression=zip", "Compression=zlib"},
        {"Compression=none", "Compression=none"},
        {"DefaultGroupName", "DefaultGroupName"},
        {"UninstallDisplayName", "UninstallDisplayName"},
    };
    for (auto& m : maps) {
        text = replace_all_str(text, m.from, m.to);
    }

    // Line-by-line: drop #preprocessor lines, accumulate [Code] body for
    // shallow parsing (but don't feed it to the ini-style parser).
    std::string out;
    std::string code_text;
    bool in_code = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        std::string raw_line = line;
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t')) line.pop_back();
        while (!line.empty() && (line.front()==' '||line.front()=='\t')) line.erase(0,1);

        if (line.rfind("[Code]", 0) == 0 || line == "[Code]") {
            in_code = true;
            pos = nl + 1;
            continue;
        }
        if (in_code) {
            if (!line.empty() && line[0] == '[') {
                in_code = false;
                // fall through: this line opens the next real section
            } else {
                code_text += raw_line;
                code_text += '\n';
                pos = nl + 1;
                continue;
            }
        }
        if (!line.empty() && line[0] == '#') { pos = nl + 1; continue; } // preprocessor
        out += line + "\n";
        pos = nl + 1;
    }

    AtuScript scr = parse_atu_text(out);

    // Shallow-parse Pascal [Code] to collect procedure/function names.
    std::vector<std::string> funcs = parse_pascal_code(code_text);
    if (!funcs.empty()) {
        std::string joined;
        for (size_t k = 0; k < funcs.size(); ++k) {
            if (k) joined += ",";
            joined += funcs[k];
        }
        scr.setup["CodeFuncs"] = joined;
        std::fprintf(stderr,
            "[.iss] [Code] 段：解析到 %zu 个过程/函数: %s\n",
            funcs.size(), joined.c_str());
    }

    // v0.2.3: load [Code] into the mini Pascal interpreter and evaluate
    // {code:FuncName} placeholders inside [Setup] values at compile time.
    if (!code_text.empty()) {
        PascalInterp interp;
        PascalContext ctx;
        ctx.app_name    = scr.get("AppName", "MyApp");
        ctx.app_version = scr.get("AppVersion", "1.0.0");
        // ExpandConstant('{#Foo}') -> [Setup] Foo value. Other macros are
        // returned unchanged (they are resolved later by the runtime).
        ctx.expand = [&scr](const std::string& a) -> std::string {
            if (a.size() >= 3 && a[0] == '{' && a[1] == '#') {
                size_t e = a.find('}');
                if (e != std::string::npos) {
                    std::string key = a.substr(2, e - 2);
                    auto it = scr.setup.find(key);
                    if (it != scr.setup.end()) return it->second;
                }
            }
            return a;
        };
        interp.set_context(ctx);
        try {
            interp.load(code_text);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[.iss] [Code] 解释器加载警告: %s\n", e.what());
        }

        // Substitute {code:Name} in every [Setup] value.
        std::vector<std::string> keys;
        keys.reserve(scr.setup.size());
        for (const auto& kv : scr.setup) keys.push_back(kv.first);
        for (const auto& k : keys) {
            std::string v = scr.setup[k];
            if (v.find("{code:") != std::string::npos) {
                std::string nv = substitute_code_refs(v, interp);
                std::fprintf(stderr, "[.iss] {code:} 替换: [%s] '%s' -> '%s'\n",
                             k.c_str(), v.c_str(), nv.c_str());
                scr.setup[k] = nv;
            }
        }
    }

    return scr;
}

} // namespace atu
