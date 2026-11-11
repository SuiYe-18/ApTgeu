/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - mini Pascal (Pascal Script subset) interpreter.
//
// Purpose: at COMPILE TIME, evaluate [Code] section functions so that
// {code:FuncName} placeholders in the .iss can be replaced by the string
// the function returns. This is NOT a full Pascal engine - it is a small
// tree-walking interpreter covering the subset actually used by installer
// scripts:
//
//   * const / var / begin / end blocks
//   * if / then / else
//   * string literals (single AND double quoted, '' escape)
//   * string concatenation with +
//   * integer / boolean / string values, comparison operators
//   * built-in string routines: Length, Copy, Pos
//   * built-ins: MsgBox (compile-time no-op), ExpandConstant (callback),
//                GetAppVersion
//
// Functions returning string are invoked by call_string(name). Unknown
// constructs are tolerated (best-effort) rather than aborting the build;
// if a function cannot be evaluated, call_string returns "".
#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <stdexcept>

namespace atu {

// Context handed to the interpreter so built-ins can reach compiler state.
struct PascalContext {
    std::string app_name;
    std::string app_version;
    // Resolve an ExpandConstant argument, e.g. "{#AppName}" -> "CodeTest".
    // The default (null) callback returns its input unchanged.
    std::function<std::string(const std::string&)> expand;
};

class PascalInterp {
public:
    PascalInterp();
    ~PascalInterp();

    // Feed the raw [Code] body. Parses top-level const/var blocks and
    // function/procedure definitions. Throws std::runtime_error only on
    // unrecoverable internal errors; unknown syntax is skipped.
    void load(const std::string& code);

    void set_context(const PascalContext& ctx);
    bool has_function(const std::string& name) const;

    // Execute a function declared as `function Name: string; ... end;`.
    // Returns the value assigned to Result (converted to string).
    // On any evaluation error returns "" and (optionally) sets err_out.
    std::string call_string(const std::string& name, std::string* err_out = nullptr);

private:
    struct Impl;
    Impl* impl_;
    PascalContext ctx_;  // stored context for set_context()/context()
};

// Replace every {code:FuncName} occurrence in s by the string returned
// by interp.call_string(FuncName). Unknown functions are replaced by ""
// (mirrors the old strip behaviour). Never throws.
std::string substitute_code_refs(const std::string& s, PascalInterp& interp);

} // namespace atu
