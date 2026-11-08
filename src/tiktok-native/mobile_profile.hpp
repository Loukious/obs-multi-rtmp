#pragma once
#include "tiktok_studio_account.hpp"
#include "live_settings.hpp"
#include <QDateTime>
#include <QJsonObject>
#include <QList>

using TikTokParams = QList<QPair<QString, QString>>;
TikTokParams mobile_request_params(const TikTokStudioAccountCredentials &account, const QString &country);
TikTokParams mobile_create_form(const QString &title, const QString &topic_id, const QString &game_id,
    const QJsonObject &profile, bool mature, qint64 timestamp_ms, bool dual_layout = false, const TikTokLiveSettings& settings = {});
QJsonObject mobile_frame_context(const QJsonObject &profile);
QByteArray mobile_user_agent(const QJsonObject &profile);
