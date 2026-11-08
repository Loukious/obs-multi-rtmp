// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QString>
#include <QUrl>

// Connection details for the hosted signer. Request signing and encoded-frame
// signing share this transport dependency, but neither subsystem depends on
// the other's payload types or media code.
struct HostedSigningServiceConfig {
	QUrl base_url = QUrl(QStringLiteral("https://tiktok-live-studio-api-signer1.p.rapidapi.com/"));
	QString api_key;

	[[nodiscard]] bool valid() const
	{
		const QString host = base_url.host().trimmed().toLower();
		const bool rapidapi_host = host == QStringLiteral("rapidapi.com") ||
			host.endsWith(QStringLiteral(".rapidapi.com"));
		return base_url.scheme() == QStringLiteral("https") && rapidapi_host &&
			base_url.userInfo().isEmpty() && base_url.query().isEmpty() &&
			base_url.fragment().isEmpty() && (base_url.port(-1) == -1 || base_url.port(-1) == 443) &&
			!api_key.trimmed().isEmpty() && api_key.size() <= 4096 &&
			!api_key.contains(QLatin1Char('\r')) && !api_key.contains(QLatin1Char('\n'));
	}
};
