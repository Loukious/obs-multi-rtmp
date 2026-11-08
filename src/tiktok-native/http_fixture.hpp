#pragma once
// Compiled only into the offline protocol test executable. A missing fixture
// is a test failure, never a fallback to a real account or network request.
#ifdef TIKTOK_NATIVE_TESTING
#include <QByteArray>
#include <QList>
#include <QHash>
#include <QUrl>
#include <functional>
#include <stdexcept>
#include <curl/curl.h>
struct TikTokHttpFixtureResponse { long status = 200; QByteArray body; QString error; QByteArray cookie_jar; QHash<QByteArray, QByteArray> headers; CURLcode curl_code = CURLE_OK; };
using TikTokHttpHeaders = QList<QPair<QByteArray, QByteArray>>;
inline std::function<TikTokHttpFixtureResponse(const QByteArray &, const QUrl &,
    const TikTokHttpHeaders &, const QByteArray &)> tiktok_http_fixture;
inline TikTokHttpFixtureResponse fixture_request(const QByteArray &method, const QUrl &url,
    const TikTokHttpHeaders &headers, const QByteArray &body)
{
    if (!tiktok_http_fixture) throw std::runtime_error("No HTTP fixture installed");
    return tiktok_http_fixture(method, url, headers, body);
}
#endif
