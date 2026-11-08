// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_studio_topics.hpp"

const QVector<TikTokStudioTopic> &tiktok_studio_topics()
{
	static const QVector<TikTokStudioTopic> topics{
		{QStringLiteral("5"), QStringLiteral("Gaming")},
		{QStringLiteral("6"), QStringLiteral("Music")},
		{QStringLiteral("42"), QStringLiteral("Chat & Interview")},
		{QStringLiteral("9"), QStringLiteral("Beauty & Fashion")},
		{QStringLiteral("3"), QStringLiteral("Dance")},
		{QStringLiteral("13"), QStringLiteral("Fitness & Sports")},
		{QStringLiteral("4"), QStringLiteral("Food")},
		{QStringLiteral("43"), QStringLiteral("News & Event")},
		{QStringLiteral("45"), QStringLiteral("Education")},
	};
	return topics;
}

bool tiktok_studio_topic_is_gaming(const QString &topic_id)
{
	return topic_id.trimmed() == QStringLiteral("5");
}
