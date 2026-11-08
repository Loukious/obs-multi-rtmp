// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Bridge Contributors

#pragma once

#include <QString>

// Loads the plugin-owned locale file selected from OBS' active UI language.
// English is used when no matching translation is bundled.
void load_translations();
QString text(const char *key);
QString translated_or(const char *key, const QString &fallback);
QString obs_language();
