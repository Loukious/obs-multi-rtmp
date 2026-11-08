#include "rapidapi_quota.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <mutex>

namespace {
std::mutex mutex;
QString directory;
QHash<QByteArray, RapidApiQuota> snapshots;

QByteArray Identity(const HostedSigningServiceConfig& service)
{
    // Separate subscriptions and providers, without persisting credentials.
    return QCryptographicHash::hash(service.base_url.host().toLower().toUtf8() + '\0' +
        service.api_key.trimmed().toUtf8(), QCryptographicHash::Sha256).toHex();
}
QString Path(const QByteArray& id) { return QDir(directory).filePath(QString::fromLatin1(id) + ".json"); }
qint64 Number(const QHash<QByteArray, QByteArray>& headers, const char* first, const char* second)
{
    for (const char* name : {first, second}) {
        bool ok = false;
        const auto number = headers.value(name).trimmed().toLongLong(&ok);
        if (ok && number >= 0) return number;
    }
    return -1;
}
}

RapidApiQuota ParseRapidApiQuota(const QHash<QByteArray, QByteArray>& headers, qint64 observed)
{
    QHash<QByteArray, QByteArray> normalized;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) normalized.insert(it.key().toLower(), it.value());
    RapidApiQuota quota;
    quota.limit = Number(normalized, "x-ratelimit-requests-limit", "x-quota-limit");
    quota.remaining = Number(normalized, "x-ratelimit-requests-remaining", "x-quota-remaining");
    const auto used = Number(normalized, "x-ratelimit-requests-used", "x-quota-used");
    if (quota.remaining < 0 && quota.limit >= 0 && used >= 0) quota.remaining = std::max(qint64(0), quota.limit - used);
    const auto reset = Number(normalized, "x-ratelimit-requests-reset", "x-quota-reset");
    // Match the original app: small reset values are seconds from observation.
    if (reset > 0) quota.reset_epoch_seconds = reset > 100000000 ? reset : observed + reset;
    if (quota.known()) quota.observed_epoch_seconds = observed;
    return quota;
}

RapidApiQuota RecordRapidApiQuota(const HostedSigningServiceConfig& service,
    const QHash<QByteArray, QByteArray>& headers)
{
    const auto quota = ParseRapidApiQuota(headers, QDateTime::currentSecsSinceEpoch());
    if (!quota.known() || service.api_key.trimmed().isEmpty()) return quota;
    std::lock_guard lock(mutex);
    const auto id = Identity(service);
    snapshots[id] = quota;
    if (!directory.isEmpty() && QDir().mkpath(directory)) {
        QSaveFile file(Path(id));
        if (file.open(QIODevice::WriteOnly)) {
            const auto json = QJsonDocument(QJsonObject{{"limit", quota.limit}, {"remaining", quota.remaining},
                {"reset", quota.reset_epoch_seconds}, {"updated", quota.observed_epoch_seconds}}).toJson(QJsonDocument::Compact);
            if (file.write(json) == json.size()) file.commit();
        }
    }
    return quota;
}

RapidApiQuota ReadRapidApiQuota(const HostedSigningServiceConfig& service)
{
    if (service.api_key.trimmed().isEmpty()) return {};
    std::lock_guard lock(mutex);
    const auto id = Identity(service);
    auto found = snapshots.constFind(id);
    if (found != snapshots.cend()) return found.value();
    RapidApiQuota quota;
    if (!directory.isEmpty()) {
        QFile file(Path(id));
        if (file.open(QIODevice::ReadOnly) && file.size() <= 4096) {
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            quota.limit = std::max(qint64(-1), object.value("limit").toInteger(-1));
            quota.remaining = std::max(qint64(-1), object.value("remaining").toInteger(-1));
            quota.reset_epoch_seconds = object.value("reset").toInteger();
            quota.observed_epoch_seconds = object.value("updated").toInteger();
            if (quota.observed_epoch_seconds <= 0) quota = {};
        }
    }
    snapshots[id] = quota;
    return quota;
}

void SetRapidApiQuotaDirectory(const QString& path)
{
    std::lock_guard lock(mutex);
    directory = path;
    snapshots.clear();
}
