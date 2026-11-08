#pragma once
#include "tiktok_studio_account.hpp"
#include <QVector>
#include <QUrl>

struct TikTokSafetyContext {
    TikTokStudioAccountCredentials account;
    QString cookiePath, userId, label;
    QJsonObject pcProfile;
    QByteArray cookieIdentity;
};
struct TikTokSafetySnapshot {
    TikTokSafetyContext context;
    QVector<QJsonObject> records;
    QString error;
};
struct TikTokAppealRoute {
    QString kind, id, submissionId, featureType;
    QUrl officialUrl;
    bool operator==(const TikTokAppealRoute& other) const {
        return kind == other.kind && id == other.id && submissionId == other.submissionId &&
            featureType == other.featureType && officialUrl == other.officialUrl;
    }
};
struct TikTokAppealState {
    TikTokSafetyContext context;
    QJsonObject record, detail;
    TikTokAppealRoute route;
    QString status, reason, error;
    bool canSubmit = false;
    bool submitted = false;
};
QString TikTokViolationKey(const QJsonObject& record);
QByteArray TikTokSafetyCookieIdentity(const QByteArray& jar);
QString TikTokViolationSummary(const QJsonObject& record);
TikTokAppealRoute TikTokViolationRoute(const QJsonObject& record);
QVector<QPair<QString, QString>> TikTokAppealReasons();
TikTokSafetySnapshot CheckTikTokViolations(TikTokSafetyContext context);
TikTokAppealState LoadTikTokAppeal(TikTokSafetyContext context, const QJsonObject& record);
TikTokAppealState SubmitTikTokAppeal(const TikTokAppealState& reviewed, const QString& reason, const QString& explanation);
