// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QByteArray>
#include <QString>

QString bind_tiktok_qr_client_secret(const QString &encoded_url,
	const QString &client_secret);
QByteArray render_tiktok_qr_png(const QString &value);
