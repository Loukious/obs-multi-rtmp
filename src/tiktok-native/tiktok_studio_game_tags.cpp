// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_studio_game_tags.hpp"

#include <QJsonArray>
#include <QSet>

QVector<TikTokStudioGameTag> parse_tiktok_studio_game_tags(const QJsonObject &payload)
{
	const QJsonArray items = payload.value(QStringLiteral("data")).toObject()
		.value(QStringLiteral("game_tag_list")).toArray();
	QVector<TikTokStudioGameTag> result;
	result.reserve(items.size());
	QSet<QString> seen_ids;
	for (const QJsonValue &value : items) {
		const QJsonObject item = value.toObject();
		const QString id = item.value(QStringLiteral("id")).toVariant().toString().trimmed();
		const QString name = item.value(QStringLiteral("show_name")).toString().trimmed();
		if (id.isEmpty() || name.isEmpty() || seen_ids.contains(id))
			continue;
		seen_ids.insert(id);
		result.push_back({id, name});
	}
	return result;
}
