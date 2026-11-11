/* v0.19: simple expression evaluator for [Setup] Conditions
 * Supports: IsWin64, IsAdmin, NeedAdmin, and/or/not, == !=
 */
#pragma once
#include <string>

namespace atu {

struct ExprCtx {
    bool is_win64 = true;
    bool is_admin = false;
};

/* Evaluate a condition string. Returns true/false. Throws on parse error. */
bool eval_expr(const std::string& expr, const ExprCtx& ctx);

} // namespace atu
