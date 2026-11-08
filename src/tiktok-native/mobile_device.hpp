#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QUrl>

// Native port of TiktokStreaming/Libs/mobile_device_gen.py and its ttEncrypt
// codec. Registration goes directly to TikTok, without the signing service.
QJsonObject complete_mobile_device_profile(QJsonObject profile);
QByteArray encrypt_mobile_registration(const QByteArray& raw, QByteArray seed = {});
struct MobileRegistrationRequest {
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
};
MobileRegistrationRequest prepare_mobile_registration(const QJsonObject& profile,
    qint64 timestampMs = -1, QString requestId = {}, const QByteArray& seed = {});
QJsonObject parse_mobile_registration(const QJsonObject& response);
