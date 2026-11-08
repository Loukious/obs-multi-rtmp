// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <QJsonObject>

// Values supplied by RapidAPI response headers. They are account-scoped,
// non-secret telemetry: a profile never needs to send an extra request merely
// to draw its usage indicator.
struct RapidApiQuota {
	// Header parsing converts relative reset intervals to Unix timestamps.
	static constexpr qint64 earliest_calendar_reset_epoch_seconds = 946684800; // 2000-01-01 UTC

	qint64 limit = -1;
	qint64 remaining = -1;
	qint64 reset_epoch_seconds = 0;
	qint64 observed_epoch_seconds = 0;

	[[nodiscard]] bool known() const { return limit >= 0 || remaining >= 0; }
	[[nodiscard]] bool has_calendar_reset() const
	{
		return reset_epoch_seconds >= earliest_calendar_reset_epoch_seconds;
	}
};

// Secret and account-scoped state for one TikTok LIVE Studio login. The
// profile INI stores only the stable account_id; this object is persisted in
// Windows Credential Manager so additional profiles/accounts can be added
// without coupling credentials to the dock UI.
struct TikTokStudioAccountCredentials {
	bool mobile = false;
	QJsonObject device_profile;
	QString rapidapi_key;
	QString signer_api_url = QStringLiteral("https://tiktok-live-studio-api-signer1.p.rapidapi.com/");
	QString device_id;
	QString install_id;
	QString live_studio_version = QStringLiteral("1.27.0");
	QString username;
	QString user_id;
	QByteArray cookie_jar;
	RapidApiQuota rapidapi_quota;
    // Runtime discovery only: environment priority is not the account's IDC.
    QString priority_region, store_region, live_webcast_host;
    bool region_checked = false;

	[[nodiscard]] bool has_device() const
	{
		return !device_id.trimmed().isEmpty() && device_id != QStringLiteral("0") &&
			!install_id.trimmed().isEmpty() && install_id != QStringLiteral("0");
	}

	[[nodiscard]] bool has_login() const
	{
		return has_device() && !cookie_jar.isEmpty();
	}
};

// A device registration is the stable identity of one saved LIVE Studio
// account. API responses are allowed to refresh cookies and account metadata,
// but they must never accidentally clear or rotate a registration that is
// already persisted. Deleting the saved account remains the explicit way to
// generate a new identity.
inline void preserve_tiktok_studio_device_identity(
	const TikTokStudioAccountCredentials &persisted,
	TikTokStudioAccountCredentials *updated)
{
	if (updated && persisted.has_device()) {
		updated->device_id = persisted.device_id;
		updated->install_id = persisted.install_id;
	}
}
