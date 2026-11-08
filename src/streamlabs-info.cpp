#include "streamlabs-info.h"
#include "tiktok-native/request_cancellation.hpp"
#include "tiktok-native/http_fixture.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrl>
#include <curl/curl.h>

namespace {
size_t AppendResponse(char *data, size_t size, size_t count, void *opaque)
{
    auto &body = *static_cast<QByteArray *>(opaque);
    constexpr size_t limit = 1024 * 1024;
    if (size && count > limit / size) return 0;
    const size_t bytes = size * count;
    if (bytes > limit - static_cast<size_t>(body.size())) return 0;
    body.append(data, static_cast<qsizetype>(bytes));
    return bytes;
}

} // namespace

StreamlabsInfoResponse FetchStreamlabsInfo(const std::string &token, const std::atomic_bool &cancelled,
    const std::optional<QString> &category)
{
    if (token.empty() || token.size() > 4096 || token.find_first_of("\r\n") != std::string::npos || token.find('\0') != std::string::npos)
        return {{}, "Account.InvalidToken"};
    if (cancelled) return {};
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return {{}, "Account.NetworkError"};
    TikTokRequestCancellation cancellation(&cancelled);
    QByteArray endpoint("https://streamlabs.com/api/v5/slobs/tiktok/info");
    if (category.has_value()) endpoint += "?category=" + QUrl::toPercentEncoding(LimitStreamlabsCategoryQuery(*category));
    const auto auth = QByteArray("Bearer ") + QByteArray::fromStdString(token);
    QByteArray body;
    long status = 0;
#ifdef TIKTOK_NATIVE_TESTING
    const auto response = fixture_request("GET", QUrl::fromEncoded(endpoint), {{"Authorization", auth}}, {});
    if (!response.error.isEmpty()) return {{}, "Account.NetworkError"};
    body = response.body;
    status = response.status;
#else
    CURL *curl = curl_easy_init();
    if (!curl) return {{}, "Account.NetworkError"};
    curl_slist *headers = curl_slist_append(nullptr, ("Authorization: " + auth).constData());
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.constData());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 Electron/29.3.1 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    set_tiktok_request_cancellation(curl);
    const auto result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) return {{}, "Account.NetworkError"};
#endif
    if (status == 401 || status == 403) return {{}, "Account.InvalidToken"};
    if (status == 429) return {{}, "Account.RateLimited"};
    if (status < 200 || status >= 300) return {{}, "Account.NetworkError"};
    const auto root = QJsonDocument::fromJson(body).object();
    if (root.value("success").isBool() && !root.value("success").toBool())
        return {{}, "Account.Unavailable"};
    if (root.isEmpty()) return {{}, "Account.Unavailable"};
    return {root};
}

StreamlabsCategoryResult SearchStreamlabsCategories(const std::string &token, const QString &query, const std::atomic_bool &cancelled)
{
    if (cancelled) return {};
    if (query.trimmed().isEmpty() || query == "Other")
        return {{{"Other", ""}}};
    const auto response = FetchStreamlabsInfo(token, cancelled, query);
    if (response.errorKey) return {{}, response.errorKey};
    const auto categories = response.data.value("categories");
    if (!categories.isArray()) return {{}, "Category.Unavailable"};
    StreamlabsCategoryResult result;
    for (const auto &entry : categories.toArray()) {
        const auto object = entry.toObject();
        const auto name = object.value("full_name").toString();
        const auto id = object.value("game_mask_id");
        if (!name.isEmpty() && id.isString() && !id.toString().isEmpty())
            result.categories.append(StreamlabsCategory{name, id.toString()});
    }
    result.categories.append(StreamlabsCategory{"Other", ""});
    return result;
}
