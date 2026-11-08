// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include "hosted_signing_service.hpp"
#include "tiktok_studio_account.hpp"

#include <QMap>
#include <QString>
#include <QJsonObject>

#include <atomic>

struct FrameSignInput {
	QString aid = QStringLiteral("8311");
	QString uid;
	QString device_id;
	QString room_id;
	QString frame_type = QStringLiteral("2");
	qint64 timestamp_seconds = 0;
	bool mobile = false;
	QJsonObject sdk_context;
};

struct FrameSignResult {
	QString frame_type;
	QString license_id;
	QString sign_info;
	QString sign_value;
	QString sign_version;

	[[nodiscard]] bool valid() const;
	[[nodiscard]] QByteArray compact_json() const;
};

struct FrameSignBatch {
	QMap<qint64, FrameSignResult> signatures;
	RapidApiQuota quota;
	QString error;

	[[nodiscard]] bool valid() const { return !signatures.isEmpty() && error.isEmpty(); }
	[[nodiscard]] qint64 first_timestamp() const { return signatures.isEmpty() ? 0 : signatures.firstKey(); }
	[[nodiscard]] qint64 last_timestamp() const { return signatures.isEmpty() ? 0 : signatures.lastKey(); }
};

// HTTPS client for the external signing service. It intentionally contains no
// frame-signing algorithm or fallback; blank/rejected signatures fail closed.
class FrameSignClient final {
public:
	static FrameSignBatch fetch_batch(const HostedSigningServiceConfig &service, const FrameSignInput &input,
		qint64 start_timestamp_seconds, int duration_seconds = 300, int step_seconds = 1,
		const std::atomic_bool *cancelled = nullptr);
	static FrameSignBatch parse_batch_response(const QByteArray &body, long http_status,
		const FrameSignInput &input);
};
