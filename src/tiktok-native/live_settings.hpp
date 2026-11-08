#pragma once
#include "../tiktok-live-settings.h"
#include <QJsonObject>
#include <QList>
#include <QStringList>

// Matches Libs/live_settings.py: AuthSwitch is 1=on, 2=off; ecommerce
// state is an enum, not the disclosure boolean.
inline QList<QPair<QString, QString>> tiktok_live_settings_fields(const TikTokLiveSettings& settings, bool mobile)
{
    const bool own = settings.contentDisclosure == "own_brand" || settings.contentDisclosure == "both";
    const bool third = settings.contentDisclosure == "third_party" || settings.contentDisclosure == "both";
    QList<QPair<QString, QString>> fields{{"gift_auth", settings.allowGifts ? "1" : "2"},
        {"chat_auth", settings.allowComments ? "1" : "2"},
        {"open_commercial_content_toggle", own || third ? "true" : "false"},
        {"commercial_content_promote_myself", own ? "true" : "false"},
        {"commercial_content_promote_third_party", third ? "true" : "false"}};
    if (third || mobile) fields.append(QPair<QString, QString>{"ecom_bc_toggle", third ? "5" : "0"});
    return fields;
}

inline QString tiktok_live_settings_conflict(const TikTokLiveSettings& settings, const QJsonObject& room)
{
    const auto auth = room.value("room_auth").toObject();
    const auto enabled = [](const QJsonValue& value) {
        return value == QJsonValue(true) || value == QJsonValue(1) || value == QJsonValue("1") || value == QJsonValue("true");
    };
    QStringList conflicts;
    if (!settings.allowGifts && enabled(auth.value("Gift"))) conflicts << "gifts";
    if (!settings.allowComments && enabled(auth.value("Chat"))) conflicts << "comments";
    return conflicts.isEmpty() ? QString() : "TikTok left " + conflicts.join(" and ") + " enabled despite your startup settings.";
}
