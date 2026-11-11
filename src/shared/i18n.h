/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * ApTgeu (ATU) - A Windows installer authoring tool
 * Copyright (c) 2026 SuiYe_TR
 */
// ApTgeu shared - i18n string tables (zh/en)
#pragma once

#include <string>

namespace atu {

// Call once at startup with the Language value from payload config ("zh" or "en").
void i18n_set_language(const std::string& lang);

// v0.16: override specific keys with custom strings (from payload config [Messages]).
void i18n_override(const char* key, const std::string& value);

// Get a localized UI string by key, UTF-8 encoded. Returns "?" if key not found.
std::string i18n(const char* key);

// Wide-char variant (for Win32 MessageBoxW / BROWSEINFOW).
std::wstring i18n_w(const char* key);

// v0.2.5: return the currently active language code (e.g. "zh", "en").
// Used by the stub to append /LANG=xx to the uninstaller's UninstallString.
std::string i18n_current_lang();

} // namespace atu
