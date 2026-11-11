// v0.2.3: mini Pascal (Pascal Script subset) interpreter.
// See pascal.h for the contract. This is a small tokenizing + tree-walking
// interpreter used at COMPILE TIME to evaluate [Code] functions so that
// {code:FuncName} placeholders can be substituted by their string result.
#include "pascal.h"

#include <cctype>
#include <cstring>
#include <algorithm>

namespace atu {
namespace {

std::string lower_of(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back((char)std::tolower((unsigned char)c));
    return r;
}

struct Tok {
    enum Type { EOF_, IDENT, NUMBER, STRING, SYM };
    Type type = EOF_;
    std::string text;   // raw text for IDENT/NUMBER/SYM; decoded value for STRING
    long num = 0;
};

std::vector<Tok> tokenize(const std::string& src) {
    std::vector<Tok> out;
    size_t i = 0, n = src.size();
    auto isidstart = [](char c) { return std::isalpha((unsigned char)c) || c == '_'; };
    auto isid      = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    while (i < n) {
        char c = src[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        // { ... } comment
        if (c == '{') {
            ++i;
            while (i < n && src[i] != '}') ++i;
            if (i < n) ++i;
            continue;
        }
        // (* ... *) comment
        if (c == '(' && i + 1 < n && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == ')')) ++i;
            if (i + 1 < n) i += 2;
            continue;
        }
        // // line comment
        if (c == '/' && i + 1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        // string literal (single or double quoted; doubled quote = escape)
        if (c == '\'' || c == '"') {
            char q = c; ++i;
            std::string val;
            while (i < n) {
                if (src[i] == q) {
                    if (i + 1 < n && src[i+1] == q) { val.push_back(q); i += 2; continue; }
                    ++i; break;
                }
                val.push_back(src[i]); ++i;
            }
            Tok t; t.type = Tok::STRING; t.text = val; out.push_back(t);
            continue;
        }
        // integer
        if (std::isdigit((unsigned char)c)) {
            long v = 0;
            while (i < n && std::isdigit((unsigned char)src[i])) { v = v*10 + (src[i]-'0'); ++i; }
            Tok t; t.type = Tok::NUMBER; t.num = v; out.push_back(t);
            continue;
        }
        // identifier
        if (isidstart(c)) {
            size_t s = i;
            while (i < n && isid(src[i])) ++i;
            Tok t; t.type = Tok::IDENT; t.text = src.substr(s, i - s); out.push_back(t);
            continue;
        }
        // two-char symbols
        if (i + 1 < n) {
            std::string two = src.substr(i, 2);
            if (two == ":=" || two == "<>" || two == "<=" || two == ">=" || two == "..") {
                Tok t; t.type = Tok::SYM; t.text = two; out.push_back(t); i += 2; continue;
            }
        }
        // single-char symbols
        if (strchr("=<>+-*/;:(),.[]", c)) {
            Tok t; t.type = Tok::SYM; t.text = std::string(1, c); out.push_back(t); ++i; continue;
        }
        ++i; // unknown char: skip
    }
    Tok eof; eof.type = Tok::EOF_; out.push_back(eof);
    return out;
}

struct Value {
    enum Type { Str, Int, Bool } type = Str;
    std::string s;
    long i = 0;
    bool b = false;
    static Value mkStr(const std::string& v) { Value x; x.type = Str; x.s = v; return x; }
    static Value mkInt(long v)               { Value x; x.type = Int; x.i = v; return x; }
    static Value mkBool(bool v)              { Value x; x.type = Bool; x.b = v; return x; }
    std::string asString() const {
        if (type == Str)  return s;
        if (type == Int)  return std::to_string(i);
        return b ? "True" : "False";
    }
    long asInt() const {
        if (type == Int)  return i;
        if (type == Bool) return b ? 1 : 0;
        try { return std::stol(s); } catch (...) { return 0; }
    }
    bool asBool() const {
        if (type == Bool) return b;
        if (type == Int)  return i != 0;
        std::string l = lower_of(s);
        return l == "true" || l == "yes" || l == "1";
    }
};

struct FuncDef {
    std::string name; // lowercased
    std::vector<std::pair<std::string,std::string>> params; // lowercased name, type
    std::string ret_type; // lowercased
    size_t body_start = 0;
    size_t body_end   = 0; // index of the closing `end` token
};

struct Engine {
    std::vector<Tok> toks;
    std::map<std::string, Value> globals;
    std::map<std::string, FuncDef> funcs;
    PascalContext ctx;
    int recursion = 0;
    std::vector<std::map<std::string, Value>> frames;

    bool is_kw(size_t p, const char* kw) const {
        return p < toks.size() && toks[p].type == Tok::IDENT && lower_of(toks[p].text) == kw;
    }
    bool is_sym(size_t p, const char* sym) const {
        return p < toks.size() && toks[p].type == Tok::SYM && toks[p].text == sym;
    }
    std::map<std::string,Value>& cur() { return frames.back(); }

    Value get_var(const std::string& name) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        auto g = globals.find(name);
        if (g != globals.end()) return g->second;
        return Value::mkStr("");
    }
    void set_var(const std::string& name, const Value& v) { cur()[name] = v; }

    // forward decls
    Value eval_expr(size_t& p, size_t end);
    size_t exec_stmt_list(size_t p, size_t end);

    // identifiers that start a new construct (end of const/var block)
    static bool is_section_kw(const std::string& id) {
        static const char* kws[] = {
            "var","const","begin","end","function","procedure","if","then",
            "else","and","or","not","div","mod","true","false","result","exit",
            "program","unit","uses","interface","implementation"
        };
        std::string l = lower_of(id);
        for (auto k : kws) if (l == k) return true;
        return false;
    }
    // parse const block into a symbol map (stops at a section keyword)
    size_t parse_const_block(size_t p, std::map<std::string,Value>& out) {
        while (p < toks.size() && toks[p].type == Tok::IDENT &&
               !is_section_kw(toks[p].text)) {
            std::string name = lower_of(toks[p].text); ++p;
            if (is_sym(p, "=")) ++p;
            Value v;
            if (p < toks.size() && toks[p].type == Tok::NUMBER) { v = Value::mkInt(toks[p].num); ++p; }
            else if (p < toks.size() && toks[p].type == Tok::STRING) { v = Value::mkStr(toks[p].text); ++p; }
            else { if (p < toks.size()) ++p; v = Value::mkStr(""); }
            if (is_sym(p, ";")) ++p;
            out[name] = v;
        }
        return p;
    }
    size_t parse_var_block(size_t p, std::map<std::string,Value>& out) {
        while (p < toks.size() && toks[p].type == Tok::IDENT &&
               !is_section_kw(toks[p].text)) {
            std::string name = lower_of(toks[p].text); ++p;
            if (is_sym(p, ":")) ++p;
            std::string type;
            if (p < toks.size() && toks[p].type == Tok::IDENT) { type = lower_of(toks[p].text); ++p; }
            if (is_sym(p, ";")) ++p;
            Value v;
            if (type == "integer" || type == "int") v = Value::mkInt(0);
            else if (type == "boolean" || type == "bool") v = Value::mkBool(false);
            else v = Value::mkStr("");
            out[name] = v;
        }
        return p;
    }

    size_t parse_func(size_t p) {
        bool is_proc = is_kw(p, "procedure");
        ++p;
        if (p >= toks.size()) return p;
        FuncDef f;
        if (toks[p].type == Tok::IDENT) { f.name = lower_of(toks[p].text); ++p; }
        if (is_sym(p, "(")) {
            ++p;
            while (p < toks.size() && !is_sym(p, ")")) {
                std::string pname;
                if (toks[p].type == Tok::IDENT) { pname = lower_of(toks[p].text); ++p; }
                if (is_sym(p, ":")) ++p;
                std::string ptype;
                if (p < toks.size() && toks[p].type == Tok::IDENT) { ptype = lower_of(toks[p].text); ++p; }
                f.params.push_back({pname, ptype});
                if (is_sym(p, ";")) ++p;
            }
            if (is_sym(p, ")")) ++p;
        }
        if (!is_proc) {
            if (is_sym(p, ":")) {
                ++p;
                if (p < toks.size() && toks[p].type == Tok::IDENT) { f.ret_type = lower_of(toks[p].text); ++p; }
            }
        }
        if (is_sym(p, ";")) ++p;
        size_t body_start = p;
        // advance to `begin`
        while (p < toks.size() && !is_kw(p, "begin")) {
            if (is_kw(p, "end")) { body_start = p; f.body_end = p; funcs[f.name] = std::move(f); return p; }
            ++p;
        }
        if (!is_kw(p, "begin")) { funcs[f.name] = std::move(f); return p; }
        int depth = 1; ++p;
        while (p < toks.size() && depth > 0) {
            if (is_kw(p, "begin")) ++depth;
            else if (is_kw(p, "end")) --depth;
            if (depth > 0) ++p;
        }
        f.body_start = body_start;
        f.body_end = p; // index of `end`
        ++p; // skip end
        if (is_sym(p, ";")) ++p;
        funcs[f.name] = std::move(f);
        return p;
    }

    void load_top() {
        size_t p = 0;
        while (p < toks.size() && toks[p].type != Tok::EOF_) {
            if (is_kw(p, "const"))  { p = parse_const_block(p + 1, globals); }
            else if (is_kw(p, "var")) { p = parse_var_block(p + 1, globals); }
            else if (is_kw(p, "function") || is_kw(p, "procedure")) { p = parse_func(p); }
            else {
                // skip unknown top-level construct up to ;
                while (p < toks.size() && !(toks[p].type == Tok::SYM && toks[p].text == ";")) ++p;
                if (p < toks.size()) ++p;
            }
        }
    }

    Value exec_func(const FuncDef& f, const std::vector<Value>& args) {
        frames.emplace_back();
        for (size_t k = 0; k < f.params.size(); ++k) {
            cur()[f.params[k].first] = (k < args.size()) ? args[k] : Value::mkStr("");
        }
        if (f.ret_type == "integer" || f.ret_type == "int")      cur()["result"] = Value::mkInt(0);
        else if (f.ret_type == "boolean" || f.ret_type == "bool") cur()["result"] = Value::mkBool(false);
        else                                                      cur()["result"] = Value::mkStr("");

        size_t p = f.body_start;
        while (p < f.body_end) {
            if (is_kw(p, "const")) p = parse_const_block(p + 1, cur());
            else if (is_kw(p, "var")) p = parse_var_block(p + 1, cur());
            else break;
        }
        if (is_kw(p, "begin")) {
            ++p;
            exec_stmt_list(p, f.body_end);
        }
        Value r = cur()["result"];
        frames.pop_back();
        return r;
    }

    size_t skip_stmt(size_t p, size_t end) {
        if (p >= end) return p;
        if (is_kw(p, "begin")) {
            int depth = 1; ++p;
            while (p < end && depth > 0) {
                if (is_kw(p, "begin")) ++depth;
                else if (is_kw(p, "end")) --depth;
                if (depth > 0) ++p;
            }
            if (p < end && is_kw(p, "end")) ++p;
            if (is_sym(p, ";")) ++p;
            return p;
        }
        int paren = 0;
        while (p < end) {
            if (toks[p].type == Tok::SYM && toks[p].text == "(") ++paren;
            else if (toks[p].type == Tok::SYM && toks[p].text == ")") { if (paren > 0) --paren; }
            else if (toks[p].type == Tok::SYM && toks[p].text == ";" && paren == 0) { ++p; break; }
            else if (is_kw(p, "end")) break;
            ++p;
        }
        return p;
    }

    size_t exec_stmt(size_t p, size_t end);

    Value eval_call(size_t& p, size_t end);

    // ---- expression grammar (precedence climbing) ----
    Value eval_or(size_t& p, size_t end) {
        Value lhs = eval_and(p, end);
        while (p < end && is_kw(p, "or")) {
            ++p; Value rhs = eval_and(p, end);
            lhs = Value::mkBool(lhs.asBool() || rhs.asBool());
        }
        return lhs;
    }
    Value eval_and(size_t& p, size_t end) {
        Value lhs = eval_cmp(p, end);
        while (p < end && is_kw(p, "and")) {
            ++p; Value rhs = eval_cmp(p, end);
            lhs = Value::mkBool(lhs.asBool() && rhs.asBool());
        }
        return lhs;
    }
    bool compare_values(const Value& a, const std::string& op, const Value& b) {
        bool numeric = (a.type == Value::Int && b.type == Value::Int);
        int c;
        if (numeric) c = (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
        else { std::string sa = a.asString(), sb = b.asString(); c = sa.compare(sb); }
        if (op == "=")  return c == 0;
        if (op == "<>") return c != 0;
        if (op == "<")  return c < 0;
        if (op == ">")  return c > 0;
        if (op == "<=") return c <= 0;
        if (op == ">=") return c >= 0;
        return false;
    }
    Value eval_cmp(size_t& p, size_t end) {
        Value lhs = eval_add(p, end);
        if (p < end && toks[p].type == Tok::SYM &&
            (toks[p].text == "=" || toks[p].text == "<>" || toks[p].text == "<" ||
             toks[p].text == ">" || toks[p].text == "<=" || toks[p].text == ">=")) {
            std::string op = toks[p].text; ++p;
            Value rhs = eval_add(p, end);
            return Value::mkBool(compare_values(lhs, op, rhs));
        }
        return lhs;
    }
    Value eval_add(size_t& p, size_t end) {
        Value lhs = eval_mul(p, end);
        while (p < end && (is_sym(p, "+") || is_sym(p, "-"))) {
            std::string op = toks[p].text; ++p;
            Value rhs = eval_mul(p, end);
            if (op == "+") {
                if (lhs.type == Value::Int && rhs.type == Value::Int) lhs = Value::mkInt(lhs.i + rhs.i);
                else lhs = Value::mkStr(lhs.asString() + rhs.asString());
            } else {
                if (lhs.type == Value::Int && rhs.type == Value::Int) lhs = Value::mkInt(lhs.i - rhs.i);
                else lhs = Value::mkInt(0);
            }
        }
        return lhs;
    }
    Value eval_mul(size_t& p, size_t end) {
        Value lhs = eval_unary(p, end);
        while (p < end && (is_sym(p, "*") || is_sym(p, "/") || is_kw(p, "div") || is_kw(p, "mod"))) {
            std::string op = (toks[p].type == Tok::SYM) ? toks[p].text : lower_of(toks[p].text);
            ++p;
            Value rhs = eval_unary(p, end);
            long a = lhs.asInt(), b = rhs.asInt();
            if (op == "*") lhs = Value::mkInt(a * b);
            else if (op == "/" || op == "div") lhs = Value::mkInt(b == 0 ? 0 : a / b);
            else lhs = Value::mkInt(b == 0 ? 0 : a % b);
        }
        return lhs;
    }
    Value eval_unary(size_t& p, size_t end) {
        if (is_sym(p, "-")) { ++p; Value v = eval_unary(p, end); return Value::mkInt(-v.asInt()); }
        if (is_kw(p, "not")) { ++p; Value v = eval_unary(p, end); return Value::mkBool(!v.asBool()); }
        return eval_primary(p, end);
    }
    Value eval_primary(size_t& p, size_t end) {
        if (p >= end) return Value::mkStr("");
        if (toks[p].type == Tok::NUMBER) { Value v = Value::mkInt(toks[p].num); ++p; return v; }
        if (toks[p].type == Tok::STRING) { Value v = Value::mkStr(toks[p].text); ++p; return v; }
        if (is_sym(p, "(")) {
            ++p;
            Value v = eval_expr(p, end);
            if (is_sym(p, ")")) ++p;
            return v;
        }
        if (is_kw(p, "true"))  { ++p; return Value::mkBool(true); }
        if (is_kw(p, "false")) { ++p; return Value::mkBool(false); }
        if (is_kw(p, "nil"))   { ++p; return Value::mkStr(""); }
        if (toks[p].type == Tok::IDENT) {
            if (p + 1 < end && is_sym(p + 1, "(")) return eval_call(p, end);
            std::string name = lower_of(toks[p].text); ++p;
            return get_var(name);
        }
        ++p;
        return Value::mkStr("");
    }
};

size_t Engine::exec_stmt_list(size_t p, size_t end) {
    while (p < end) {
        if (is_sym(p, ";")) { ++p; continue; }
        if (is_kw(p, "end")) break;
        p = exec_stmt(p, end);
    }
    return p;
}

Value Engine::eval_expr(size_t& p, size_t end) { return eval_or(p, end); }

Value Engine::eval_call(size_t& p, size_t end) {
    std::string name = lower_of(toks[p].text); ++p;
    std::vector<Value> args;
    if (is_sym(p, "(")) {
        ++p;
        while (p < end && !is_sym(p, ")")) {
            args.push_back(eval_expr(p, end));
            if (is_sym(p, ",")) ++p;
        }
        if (is_sym(p, ")")) ++p;
    }
    // built-ins
    if (name == "length") {
        std::string s = args.empty() ? "" : args[0].asString();
        return Value::mkInt((long)s.size());
    }
    if (name == "copy") {
        std::string s = args.size() > 0 ? args[0].asString() : "";
        long start = args.size() > 1 ? args[1].asInt() : 1;
        long count = args.size() > 2 ? args[2].asInt() : -1;
        long idx = start - 1; if (idx < 0) idx = 0;
        if (idx > (long)s.size()) idx = (long)s.size();
        long len = (count < 0) ? (long)s.size() - idx : count;
        if (len < 0) len = 0;
        if (idx + len > (long)s.size()) len = (long)s.size() - idx;
        return Value::mkStr(s.substr((size_t)idx, (size_t)len));
    }
    if (name == "pos") {
        std::string sub = args.size() > 0 ? args[0].asString() : "";
        std::string s   = args.size() > 1 ? args[1].asString() : "";
        size_t f = s.find(sub);
        return Value::mkInt(f == std::string::npos ? 0 : (long)f + 1);
    }
    if (name == "msgbox") {
        // compile-time no-op: we never show a dialog while packing.
        return Value::mkInt(1); // mrOk
    }
    if (name == "expandconstant") {
        std::string a = args.empty() ? "" : args[0].asString();
        if (ctx.expand) return Value::mkStr(ctx.expand(a));
        return Value::mkStr(a);
    }
    if (name == "getappversion") return Value::mkStr(ctx.app_version);
    if (name == "getappname")    return Value::mkStr(ctx.app_name);
    if (name == "inttostr" || name == "tostr") return Value::mkStr(args.size() > 0 ? args[0].asString() : "");
    if (name == "trim") {
        std::string s = args.empty() ? "" : args[0].asString();
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return Value::mkStr(b == std::string::npos ? "" : s.substr(b, e - b + 1));
    }
    // user-defined function
    auto it = funcs.find(name);
    if (it != funcs.end()) {
        if (recursion > 32) return Value::mkStr("");
        recursion++;
        Value r = exec_func(it->second, args);
        recursion--;
        return r;
    }
    return Value::mkStr("");
}

size_t Engine::exec_stmt(size_t p, size_t end) {
    if (p >= end) return p;
    if (is_kw(p, "begin")) {
        ++p;
        p = exec_stmt_list(p, end);
        if (is_kw(p, "end")) ++p;
        if (is_sym(p, ";")) ++p;
        return p;
    }
    if (is_kw(p, "if")) {
        ++p;
        Value cond = eval_expr(p, end);
        if (is_kw(p, "then")) ++p;
        bool take_then = cond.asBool();
        if (take_then) p = exec_stmt(p, end);
        else           p = skip_stmt(p, end);
        if (is_kw(p, "else")) {
            ++p;
            if (!take_then) p = exec_stmt(p, end);
            else            p = skip_stmt(p, end);
        }
        return p;
    }
    if (toks[p].type == Tok::IDENT) {
        size_t q = p + 1;
        if (is_sym(q, ":=")) {
            std::string name = lower_of(toks[p].text);
            p = q + 1;
            Value v = eval_expr(p, end);
            set_var(name, v);
            if (is_sym(p, ";")) ++p;
            return p;
        }
        // procedure/function call statement
        Value r = eval_call(p, end);
        (void)r;
        if (is_sym(p, ";")) ++p;
        return p;
    }
    ++p;
    return p;
}

} // namespace

struct PascalInterp::Impl { Engine eng; };

PascalInterp::PascalInterp() : impl_(new Impl()) {}
PascalInterp::~PascalInterp() { delete impl_; }

void PascalInterp::set_context(const PascalContext& ctx) { impl_->eng.ctx = ctx; }

void PascalInterp::load(const std::string& code) {
    // Guard: very large or complex [Code] blocks can trigger bad_alloc.
    // Tokenize always (needed for function-name scanning), but wrap load_top
    // in try-catch so complex code doesn't crash the compiler.
    impl_->eng.toks = tokenize(code);
    try {
        impl_->eng.load_top();
    } catch (...) {
        // Parser hit something it can't handle; shallow scan already got
        // the function names. Continue with empty function table.
    }
}

bool PascalInterp::has_function(const std::string& name) const {
    return impl_->eng.funcs.count(lower_of(name)) > 0;
}

std::string PascalInterp::call_string(const std::string& name, std::string* err_out) {
    std::string ln = lower_of(name);
    auto it = impl_->eng.funcs.find(ln);
    if (it == impl_->eng.funcs.end()) {
        if (err_out) *err_out = "unknown function: " + name;
        return "";
    }
    try {
        Value r = impl_->eng.exec_func(it->second, {});
        return r.asString();
    } catch (const std::exception& e) {
        if (err_out) *err_out = e.what();
        return "";
    }
}

std::string substitute_code_refs(const std::string& s, PascalInterp& interp) {
    std::string out;
    out.reserve(s.size());
    size_t k = 0;
    while (k < s.size()) {
        if (s.compare(k, 6, "{code:") == 0) {
            size_t e = s.find('}', k + 6);
            if (e == std::string::npos) { out.append(s, k, std::string::npos); break; }
            std::string fname = s.substr(k + 6, e - (k + 6));
            out += interp.call_string(fname);
            k = e + 1;
        } else {
            out += s[k++];
        }
    }
    return out;
}

} // namespace atu
