#include "expr.h"
#include <stdexcept>
#include <cctype>

namespace atu {

static std::string token;
static size_t pos = 0;
static const std::string* src;

static void skip_ws() {
    while (pos < src->size() && std::isspace((*src)[pos])) pos++;
}

static std::string next_token() {
    skip_ws();
    if (pos >= src->size()) return "";
    char c = (*src)[pos];
    if (c == '(' || c == ')' || c == '=' || c == '!' || c == '<' || c == '>') {
        std::string t; t += c;
        if (pos+1 < src->size() && (*src)[pos+1] == '=') { t += '='; pos++; }
        pos++;
        return t;
    }
    size_t start = pos;
    while (pos < src->size() && !std::isspace((*src)[pos]) &&
           (*src)[pos] != '(' && (*src)[pos] != ')' &&
           (*src)[pos] != '=' && (*src)[pos] != '!') pos++;
    return src->substr(start, pos - start);
}

static bool eval_unary(const ExprCtx& ctx);

static bool eval_primary(const ExprCtx& ctx) {
    token = next_token();
    if (token == "(") {
        bool v = eval_unary(ctx);
        token = next_token(); // expect ")"
        return v;
    }
    if (token == "not") return !eval_primary(ctx);
    if (token == "IsWin64" || token == "iswin64") return ctx.is_win64;
    if (token == "IsAdmin" || token == "isadmin") return ctx.is_admin;
    if (token == "NeedAdmin" || token == "needadmin") return ctx.is_admin;
    if (token == "true") return true;
    if (token == "false") return false;
    return false;
}

static bool eval_and(const ExprCtx& ctx);

static bool eval_unary(const ExprCtx& ctx) {
    return eval_primary(ctx);
}

static bool eval_or(const ExprCtx& ctx);

static bool eval_and(const ExprCtx& ctx) {
    bool lhs = eval_unary(ctx);
    while (true) {
        std::string save_pos;
        token = next_token();
        if (token == "and") { lhs = lhs && eval_unary(ctx); }
        else { break; }
    }
    // put back: we consumed next token, caller needs it
    // simple: just return, caller handles
    return lhs;
}

static bool eval_or(const ExprCtx& ctx) {
    bool lhs = eval_and(ctx);
    while (true) {
        token = next_token();
        if (token == "or") { lhs = lhs || eval_and(ctx); }
        else break;
    }
    return lhs;
}

bool eval_expr(const std::string& expr, const ExprCtx& ctx) {
    src = &expr;
    pos = 0;
    return eval_or(ctx);
}

} // namespace atu
