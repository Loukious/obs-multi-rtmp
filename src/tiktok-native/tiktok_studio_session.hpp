// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// LIVE Studio selects API hosts dynamically. The public reference app loads
// saved cookies as name/value pairs, so authenticated cookies are available to
// each TikTok API host rather than remaining tied to the host that set them.
[[nodiscard]] bool is_tiktok_session_host(const QString &host);
[[nodiscard]] QByteArray tiktok_session_cookie_header(const QByteArray &netscape_cookie_jar);
[[nodiscard]] bool tiktok_studio_session_is_stale_error(const QString &error);
[[nodiscard]] bool tiktok_studio_session_requires_login(const QString &error);
[[nodiscard]] bool tiktok_studio_session_has_no_live_auth(const QString &error);

// Tolerant protocol readers shared by login, account verification, and LIVE
// creation. TikTok has returned these identifiers both directly and in nested
// user/room objects across LIVE Studio versions.
[[nodiscard]] QString tiktok_studio_account_user_id(const QJsonObject &account_data);
[[nodiscard]] QJsonObject tiktok_studio_room_object(const QJsonObject &data);
[[nodiscard]] QString tiktok_studio_room_owner_id(const QJsonObject &room_data);
