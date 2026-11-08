#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <optional>
#include <algorithm>
#include <string>

// Matches Stream.search() in StreamLabsTikTokStreamKeyGenerator: 25 Unicode
// characters in the query, without truncating displayed category names.
inline QString LimitStreamlabsCategoryQuery(const QString &text)
{
    qsizetype end = 0;
    for (int count = 0; count < 25 && end < text.size(); ++count) {
        if (text.at(end).isHighSurrogate() && end + 1 < text.size() && text.at(end + 1).isLowSurrogate())
            ++end;
        ++end;
    }
    return text.left(end);
}

// Streamlabs wraps TikTok's specific rejection in data.message; the outer
// message may only say that an unexpected connection error occurred.
inline QString StreamlabsApiError(const QJsonObject &response)
{
    const auto detail = response.value("data").toObject().value("message").toString().trimmed();
    if (!detail.isEmpty()) return detail;
    const auto message = response.value("message").toString().trimmed();
    return message.isEmpty() ? QStringLiteral("Unknown error occurred") : message;
}

struct StreamlabsInfoResponse { QJsonObject data; const char *errorKey = nullptr; };
StreamlabsInfoResponse FetchStreamlabsInfo(const std::string &token, const std::atomic_bool &cancelled,
    const std::optional<QString> &category = {});

struct StreamlabsCategory { QString name, id; };
struct StreamlabsCategoryResult { QVector<StreamlabsCategory> categories; const char *errorKey = nullptr; };
using StreamlabsCategoryLookup = std::function<StreamlabsCategoryResult(const std::string &, const QString &, const std::atomic_bool &)>;
StreamlabsCategoryResult SearchStreamlabsCategories(const std::string &token, const QString &query, const std::atomic_bool &cancelled);
