// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include "hosted_signing_service.hpp"
#include "tiktok_studio_account.hpp"

#include <QByteArray>
#include <QString>
#include <QJsonObject>

struct TikTokRequestSignatureInput {
	bool mobile = false;
	QString url, install_id;
	QJsonObject headers;
	QByteArray body;
	qint64 timestamp_seconds = 0;
	QString aid = QStringLiteral("8311");
	QString device_id;
	QByteArray encoded_query;
	QByteArray body_stub;
};

struct TikTokRequestSignatureHeaders {
	QByteArray x_khronos;
	QByteArray x_ladon;
	QByteArray x_argus;
	QByteArray x_gorgon;
	RapidApiQuota quota;
	QString error;
	bool transient = false;

	[[nodiscard]] bool valid() const
	{
		return !x_khronos.isEmpty() && !x_ladon.isEmpty() && !x_argus.isEmpty() && error.isEmpty();
	}
};

class RapidApiRequestSigner final {
public:
	static TikTokRequestSignatureHeaders fetch(const HostedSigningServiceConfig &service,
		const TikTokRequestSignatureInput &input);
	static TikTokRequestSignatureHeaders parse_response(const QByteArray &body, long http_status);
};
