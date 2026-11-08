// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QString>
#include <QVector>

struct TikTokStudioTopic {
	QString id;
	QString name;
};

[[nodiscard]] const QVector<TikTokStudioTopic> &tiktok_studio_topics();
[[nodiscard]] bool tiktok_studio_topic_is_gaming(const QString &topic_id);
