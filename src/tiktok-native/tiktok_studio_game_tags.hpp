// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

struct TikTokStudioGameTag {
	QString id;
	QString name;
};

[[nodiscard]] QVector<TikTokStudioGameTag> parse_tiktok_studio_game_tags(
	const QJsonObject &payload);
