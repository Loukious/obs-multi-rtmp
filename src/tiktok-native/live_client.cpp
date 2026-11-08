// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors
// Adapted for obs-multi-rtmp: synchronous session API and Android support.
#include "live_client.hpp"
#include "tiktok_request_signer.hpp"
#include "tiktok_studio_device.hpp"
#include "tiktok_studio_session.hpp"
#include "mobile_profile.hpp"
#include "mobile_device.hpp"
#include "request_cancellation.hpp"
#include "http_fixture.hpp"
#include "http_retry.hpp"
#include "domain_routing.hpp"
#include "api_error.hpp"
#include "qr_login.hpp"
#include "tiktok_studio_qr.hpp"
#include <curl/curl.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRandomGenerator>
#include <QTimeZone>
#include <QUrlQuery>
#include <QUuid>
#include <QFile>
#include <QHash>
#include <QDir>
#include <QTemporaryFile>
#include <algorithm>
#include <optional>
#include <mutex>
namespace {
using Params = QList<QPair<QString, QString>>;
using Headers = QList<QPair<QByteArray, QByteArray>>;
constexpr qsizetype http_response_limit = 16 * 1024 * 1024;
constexpr auto minimum_studio_version = "1.27.0";
struct HttpResult {
	long status = 0;
	QByteArray body;
	QString error;
	CURLcode curl_code = CURLE_OK;
	bool overflow = false;
};

size_t append_response(char *data, size_t size, size_t count, void *user_data)
{
	auto *result = static_cast<HttpResult *>(user_data);
	const size_t bytes = size * count;
	if (bytes > static_cast<size_t>(http_response_limit) ||
		result->body.size() > http_response_limit - static_cast<qsizetype>(bytes)) {
		result->overflow = true;
		return 0;
	}
	result->body.append(data, static_cast<qsizetype>(bytes));
	return bytes;
}

class CurlSession final {
public:
	explicit CurlSession(const QByteArray &cookie_jar = {}) : handle_(curl_easy_init())
	{
		if (!handle_)
			return;
		curl_easy_setopt(handle_, CURLOPT_COOKIEFILE, "");
		for (const QByteArray &line : cookie_jar.split('\n')) {
			if (!line.trimmed().isEmpty())
				curl_easy_setopt(handle_, CURLOPT_COOKIELIST, line.constData());
		}
	}

	~CurlSession()
	{
		if (handle_)
			curl_easy_cleanup(handle_);
	}

	CurlSession(const CurlSession &) = delete;
	CurlSession &operator=(const CurlSession &) = delete;

	[[nodiscard]] bool valid() const { return handle_ != nullptr; }

	HttpResult request(const QByteArray &method, const QByteArray &url, const Headers &headers,
		const QByteArray &body, const QByteArray &user_agent, long timeout_ms = 20000)
	{
		HttpResult result;
		if (tiktok_cancel_requested()) { result.error = "TikTok request cancelled."; return result; }
		if (!handle_) {
			result.error = QStringLiteral("libcurl could not create a TikTok request.");
			return result;
		}
		const QUrl parsed = QUrl::fromEncoded(url);
		if (parsed.scheme() != QStringLiteral("https") || parsed.host().isEmpty()) {
			result.error = QStringLiteral("TikTok returned an invalid HTTPS endpoint.");
			return result;
		}

#ifdef TIKTOK_NATIVE_TESTING
		const auto mock = fixture_request(method, parsed, headers, body);
		result.status = mock.status;
		result.body = mock.body;
		result.error = mock.error;
		result.curl_code = mock.curl_code;
		if (result.error.isEmpty() && mock.curl_code != CURLE_OK)
			result.error = QString::fromLatin1(curl_easy_strerror(mock.curl_code));
        for (const auto& line : mock.cookie_jar.split('\n'))
            if (!line.trimmed().isEmpty()) curl_easy_setopt(handle_, CURLOPT_COOKIELIST, line.constData());
		return result;
#endif

		curl_slist *header_list = nullptr;
		bool has_cookie_header = false;
		for (const auto &[name, value] : headers) {
			has_cookie_header = has_cookie_header || name.compare("Cookie", Qt::CaseInsensitive) == 0;
			header_list = curl_slist_append(header_list, (name + ": " + value).constData());
		}
		if (!has_cookie_header && is_tiktok_session_host(parsed.host())) {
			const QByteArray cookie_header = tiktok_session_cookie_header(cookies());
			if (!cookie_header.isEmpty())
				header_list = curl_slist_append(header_list,
					(QByteArrayLiteral("Cookie: ") + cookie_header).constData());
		}
		curl_easy_setopt(handle_, CURLOPT_URL, url.constData());
		curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, header_list);
		curl_easy_setopt(handle_, CURLOPT_USERAGENT, user_agent.constData());
		curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, 2L);
		curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
		curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS, timeout_ms);
		curl_easy_setopt(handle_, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, append_response);
		curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &result);
		curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
		if (method == "POST") {
			curl_easy_setopt(handle_, CURLOPT_POST, 1L);
			curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.isEmpty() ? "" : body.constData());
			curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
		} else {
			curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
		}
		set_tiktok_request_cancellation(handle_);
		const CURLcode code = curl_easy_perform(handle_);
		result.curl_code = code;
		if (code != CURLE_OK)
			result.error = result.overflow ? QStringLiteral("TikTok returned an oversized response.")
				: QString::fromLatin1(curl_easy_strerror(code));
		curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &result.status);
		curl_slist_free_all(header_list);
		return result;
	}

	[[nodiscard]] QByteArray cookies() const
	{
		QByteArray result;
		if (!handle_)
			return result;
		curl_slist *cookies = nullptr;
		if (curl_easy_getinfo(handle_, CURLINFO_COOKIELIST, &cookies) != CURLE_OK)
			return result;
		for (curl_slist *item = cookies; item; item = item->next) {
			if (!item->data)
				continue;
			if (!result.isEmpty())
				result += '\n';
			result += item->data;
		}
		curl_slist_free_all(cookies);
		return result;
	}

private:
	CURL *handle_ = nullptr;
};

QByteArray encode_params(const Params &params)
{
	QByteArray query;
	for (const auto &[name, value] : params)
	{
		if (!query.isEmpty()) query += '&';
		query += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
	}
	return query;
}

QByteArray request_url(const QString &base, const QByteArray &query)
{
	QByteArray url = base.toUtf8();
	if (!query.isEmpty())
		url += (url.contains('?') ? '&' : '?') + query;
	return url;
}

QString random_text(int length, const QByteArray &alphabet)
{
	QString result;
	result.reserve(length);
	for (int index = 0; index < length; ++index)
		result += QLatin1Char(alphabet.at(QRandomGenerator::global()->bounded(alphabet.size())));
	return result;
}

QList<int> version_parts(const QString &version)
{
	QList<int> parts;
	for (const QString &part : version.split(QLatin1Char('.')))
		parts.push_back(part.toInt());
	while (parts.size() < 3)
		parts.push_back(0);
	return parts.mid(0, 3);
}

QString normalized_version(const QString &candidate)
{
	const QList<int> found = version_parts(candidate);
	const QList<int> minimum = version_parts(QString::fromLatin1(minimum_studio_version));
	return std::lexicographical_compare(found.begin(), found.end(), minimum.begin(), minimum.end())
		? QString::fromLatin1(minimum_studio_version) : candidate;
}

QString studio_browser_version(const QString &version)
{
	return QStringLiteral("5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
		"(KHTML, like Gecko) TikTokLIVEStudio/%1 Chrome/136.0.7103.59 "
		"Electron/36.4.0-alpha.17 TTElectron/36.4.0-alpha.17 Safari/537.36").arg(version);
}

QByteArray studio_user_agent(const QString &version)
{
	return QByteArray("Mozilla/") + studio_browser_version(version).toUtf8();
}

Headers common_headers(const QString &)
{
	return {{"Accept", "application/json, text/plain, */*"},
		{"Accept-Language", "en-US"}, {"Sec-CH-UA", "\"Not.A/Brand\";v=\"99\", \"Chromium\";v=\"136\""},
		{"Sec-CH-UA-Mobile", "?0"}, {"Sec-CH-UA-Platform", "\"Windows\""},
		{"Sec-Fetch-Dest", "empty"}, {"Sec-Fetch-Mode", "cors"},
		{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Storage-Access", "active"},
		{"X-SS-DP", ""}, {"sdk_aid", "8311"}};
}

QJsonObject response_object(const HttpResult &response, const QString &action, QString *error)
{
	QJsonParseError parse_error;
	const QJsonDocument document = QJsonDocument::fromJson(response.body, &parse_error);
	// TikTok occasionally closes a successful response before satisfying the
	// advertised Content-Length. libcurl reports CURLE_PARTIAL_FILE even when
	// the complete JSON object has already arrived; use it only when parsing
	// proves that the body is intact.
	const bool usable_partial_response = response.curl_code == CURLE_PARTIAL_FILE &&
		response.status >= 200 && response.status < 300 && parse_error.error == QJsonParseError::NoError &&
		document.isObject();
	if (!response.error.isEmpty() && !usable_partial_response) {
		*error = QStringLiteral("%1 failed: %2").arg(action, response.error);
		return {};
	}
	if (response.status < 200 || response.status >= 300) {
		const auto detail = tiktok_api_message(document.object());
		*error = detail.isEmpty() ? QStringLiteral("%1 failed (HTTP %2).").arg(action).arg(response.status)
			: QStringLiteral("%1 failed: %2 (HTTP %3).").arg(action, detail).arg(response.status);
		return {};
	}
	if (!document.isObject()) {
		*error = QStringLiteral("%1 returned invalid JSON.").arg(action);
		return {};
	}
	return document.object();
}

QString webcast_error(const QJsonObject &payload, const QString &action)
{
	QString status = payload.value(QStringLiteral("status_code")).toVariant().toString();
    const auto nestedStatus = payload.value("data").toObject().value("error_code").toVariant().toString();
    if (!nestedStatus.isEmpty() && nestedStatus != "0") status = nestedStatus;
    if (status.isEmpty() && payload.value("success").isBool() && !payload.value("success").toBool()) status = "unsuccessful";
	if (status.isEmpty() || status == QStringLiteral("0") || status == QStringLiteral("4003150"))
		return {};
	const auto message = tiktok_api_message(payload);
	return message.isEmpty() ? QStringLiteral("%1 failed (TikTok status %2).").arg(action, status)
		: QStringLiteral("%1 failed: %2 (TikTok status %3).").arg(action, message, status);
}

QByteArray cookie_value(const QByteArray& jar, const QByteArray& name) { return tiktok_cookie_value(jar, name); }

struct DispatchEntry { TikTokDispatchMaps maps; qint64 expires = 0; };
struct RegionEntry { QString priority, store; qint64 expires = 0; };
std::mutex routing_mutex;
QHash<QByteArray, DispatchEntry> dispatch_cache;
QHash<QByteArray, RegionEntry> region_cache;

TikTokDispatchMaps discover_domains(const TikTokStudioAccountCredentials& account)
{
    const auto key = tiktok_routing_cache_key(account);
    const auto now = QDateTime::currentSecsSinceEpoch();
    {
        std::lock_guard lock(routing_mutex);
        const auto found = dispatch_cache.constFind(key);
        if (found != dispatch_cache.cend() && found->expires > now) return found->maps;
    }
    const auto query = encode_params(tiktok_dispatch_params(account));
    const auto agent = account.mobile ? mobile_user_agent(account.device_profile) : studio_user_agent(account.live_studio_version);
    CurlSession discovery;
    TikTokDispatchMaps maps;
    for (const char* endpoint : {"https://tnc0-normal-alisg.tiktokv.com/get_domains/v4/",
        "https://tnc16-platform-useast1a.tiktokv.com/get_domains/v4/", "https://tnc16-platform-alisg.tiktokv.com/get_domains/v4/"}) {
        if (tiktok_cancel_requested()) return {};
        const auto response = discovery.request("GET", request_url(endpoint, query), {{"Accept", "application/json"}}, {}, agent, 12000);
        if (!response.error.isEmpty() || response.status < 200 || response.status >= 300) continue;
        maps = tiktok_dispatch_maps(QJsonDocument::fromJson(response.body).object());
        if (!maps.isEmpty()) break;
    }
    {
        std::lock_guard lock(routing_mutex);
        if (dispatch_cache.size() >= 128) dispatch_cache.clear();
        // Cache a failed discovery briefly so each fallback/heartbeat does not
        // immediately repeat all three requests. Successful routes live 5 min.
        dispatch_cache.insert(key, {maps, now + (maps.isEmpty() ? 30 : 300)});
    }
    return maps;
}
QStringList api_hosts(const TikTokStudioAccountCredentials& account)
{
    const auto maps = tiktok_authenticated_host(account, true).isEmpty() ? discover_domains(account) : TikTokDispatchMaps{};
    return tiktok_host_candidates(account, true, maps);
}
QStringList webcast_hosts(const TikTokStudioAccountCredentials& account)
{
    if (!account.live_webcast_host.isEmpty()) return {account.live_webcast_host};
    // Saved login IDC takes precedence over discovery and priority_region.
    const auto maps = tiktok_authenticated_host(account, false).isEmpty() ? discover_domains(account) : TikTokDispatchMaps{};
    return tiktok_host_candidates(account, false, maps);
}
constexpr auto passport_app_key = "884c28a44b61b78f9d837fc8b0967178";

Params passport_signed(Params params, const Params &body = {})
{
	QStringList keys;
	for (const auto &[key, value] : params) {
		if (!value.isNull() && key != QStringLiteral("sign") && key != QStringLiteral("qs") &&
			key != QStringLiteral("isResend") && key != QStringLiteral("next") &&
			key != QStringLiteral("baseURL") && key != QStringLiteral("extra_params"))
			keys.push_back(key);
	}
	keys.removeDuplicates();
	std::sort(keys.begin(), keys.end());
	if (keys.size() > 10)
		keys = keys.mid(0, 10);
	auto value_for = [](const Params &items, const QString &key) {
		for (const auto &item : items)
			if (item.first == key)
				return item.second;
		return QString{};
	};
	QStringList param_parts;
	for (const QString &key : keys)
		param_parts << key + QLatin1Char('=') + value_for(params, key);
	QStringList body_keys;
	for (const auto &[key, value] : body)
		if (!value.isNull())
			body_keys << key;
	body_keys.removeDuplicates();
	std::sort(body_keys.begin(), body_keys.end());
	QStringList body_parts;
	for (const QString &key : body_keys)
		body_parts << key + QLatin1Char('=') + value_for(body, key);
	const QByteArray base = (param_parts.join(QLatin1Char('&')) + QLatin1Char('&') +
		body_parts.join(QLatin1Char('&')) + QStringLiteral("&app_key=") +
		QString::fromLatin1(passport_app_key)).toUtf8();
	const QString sign = QString::fromLatin1(QCryptographicHash::hash(base, QCryptographicHash::Md5).toHex());
	const QByteArray joined = keys.join(QLatin1Char(',')).toUtf8();
	QByteArray qs;
	qs.reserve(joined.size() * 2);
	for (const char byte : joined)
		qs += QByteArray::number(static_cast<uint8_t>(byte) ^ 5, 16).rightJustified(2, '0');
	params.push_back({QStringLiteral("sign"), sign});
	params.push_back({QStringLiteral("qs"), QString::fromLatin1(qs)});
	return params;
}

Params studio_params(const TikTokStudioAccountCredentials &account)
{
	const auto priority = !account.priority_region.isEmpty() ? account.priority_region : account.mobile
        ? tiktok_country_code(account.device_profile.value("request_params").toObject().value("priority_region").toString()) : QString();
	if (account.mobile)
		return mobile_request_params(account, priority);
	QString sdk = account.live_studio_version;
	sdk.remove(QLatin1Char('.'));
	const QString locale = QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-'));
	const QString language = QLocale::system().name().section(QLatin1Char('_'), 0, 0).toLower();
	const QString timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
	return {{QStringLiteral("aid"), QStringLiteral("8311")},
		{QStringLiteral("app_name"), QStringLiteral("tiktok_live_studio")},
		{QStringLiteral("device_id"), account.device_id}, {QStringLiteral("install_id"), account.install_id},
		{QStringLiteral("channel"), QStringLiteral("studio")},
		{QStringLiteral("version_code"), account.live_studio_version},
		{QStringLiteral("device_platform"), QStringLiteral("windows")},
		{QStringLiteral("timezone_name"), timezone},
		{QStringLiteral("screen_width"), QStringLiteral("1920")},
		{QStringLiteral("screen_height"), QStringLiteral("1080")},
		{QStringLiteral("browser_language"), locale},
		{QStringLiteral("browser_platform"), QStringLiteral("Win32")},
		{QStringLiteral("browser_name"), QStringLiteral("Mozilla")},
		{QStringLiteral("browser_version"), studio_browser_version(account.live_studio_version)},
		{QStringLiteral("language"), language},
		{QStringLiteral("app_language"), language},
		{QStringLiteral("webcast_language"), language},
		{QStringLiteral("priority_region"), priority},
		{QStringLiteral("webcast_sdk_version"), sdk}, {QStringLiteral("live_mode"), QStringLiteral("6")}};
}

struct SignedRequestResult {
	QJsonObject object;
	QString error;
    bool retryable = false;
    bool transient = false;
};

SignedRequestResult signed_request(CurlSession &session, TikTokStudioAccountCredentials &account,
	const QByteArray &method, const QString &url, const Params &params, const QByteArray &body = {},
	const QByteArray &content_type = "application/x-www-form-urlencoded; charset=UTF-8",
	const Headers &extra_headers = {}, long timeout_ms = 20000)
{
	SignedRequestResult result;
	if (tiktok_cancel_requested()) { result.error = "TikTok request cancelled."; return result; }
	const QByteArray query = encode_params(params);
	TikTokRequestSignatureInput input;
	input.timestamp_seconds = QDateTime::currentSecsSinceEpoch();
	input.device_id = account.device_id;
	input.mobile = account.mobile;
	input.aid = account.mobile ? "1233" : "8311";
	input.install_id = account.install_id;
	input.url = QString::fromUtf8(request_url(url, query));
	input.body = body;
	if (account.mobile) {
		input.headers = {{"user-agent", QString::fromUtf8(mobile_user_agent(account.device_profile))},
			{"accept", "application/json"}, {"accept-language", "en-US"},
			{"content-type", QString::fromUtf8(content_type)}, {"sdk_aid", "1233"}, {"X-SS-DP", "1233"}};
	}
	input.encoded_query = query;
	if (!body.isEmpty())
		input.body_stub = QCryptographicHash::hash(body, QCryptographicHash::Md5).toHex();
	HostedSigningServiceConfig service;
	service.base_url = QUrl(account.signer_api_url);
	service.api_key = account.rapidapi_key;
	const TikTokRequestSignatureHeaders signatures = RapidApiRequestSigner::fetch(service, input);
	if (signatures.quota.known()) {
		if (signatures.quota.limit >= 0)
			account.rapidapi_quota.limit = signatures.quota.limit;
		if (signatures.quota.remaining >= 0)
			account.rapidapi_quota.remaining = signatures.quota.remaining;
		if (!account.rapidapi_quota.has_calendar_reset())
			account.rapidapi_quota.reset_epoch_seconds = 0;
		if (signatures.quota.has_calendar_reset())
			account.rapidapi_quota.reset_epoch_seconds = signatures.quota.reset_epoch_seconds;
		account.rapidapi_quota.observed_epoch_seconds = signatures.quota.observed_epoch_seconds;
	}
	if (!signatures.valid()) {
		result.error = signatures.error;
		result.transient = signatures.transient;
		return result;
	}
	Headers headers = common_headers(account.live_studio_version);
	if (account.mobile) {
		headers.clear();
		for (auto it = input.headers.begin(); it != input.headers.end(); ++it)
			headers.append({it.key().toUtf8(), it.value().toString().toUtf8()});
		headers.push_back({"X-Gorgon", signatures.x_gorgon});
	}
	headers += extra_headers;
	headers.push_back({"Content-Type", content_type});
	headers.push_back({"Pragma", "no-cache"});
	headers.push_back({"Cache-Control", "no-cache"});
	headers.push_back({"X-Khronos", signatures.x_khronos});
	headers.push_back({"X-Ladon", signatures.x_ladon});
	headers.push_back({"X-Argus", signatures.x_argus});
	if (!input.body_stub.isEmpty())
		headers.push_back({"X-SS-Stub", input.body_stub});
	const QString region = account.store_region.isEmpty() ? tiktok_country_code(QString::fromUtf8(cookie_value(account.cookie_jar, "store-country-code"))) : account.store_region;
	if (!region.isEmpty())
		headers.push_back({"X-TT-Store-Region", region.toUtf8()});
	const HttpResult response = session.request(method, request_url(url, query), headers, body,
		account.mobile ? mobile_user_agent(account.device_profile) : studio_user_agent(account.live_studio_version), timeout_ms);
	result.object = response_object(response, QStringLiteral("TikTok request"), &result.error);
    result.transient = !result.error.isEmpty() &&
        (tiktok_transient_transport_error(response.curl_code) || tiktok_transient_http_status(response.status));
    result.retryable = !result.error.isEmpty() && (!response.error.isEmpty() || response.status == 403 || response.status == 404 || response.status == 421 ||
        response.status == 429 || response.status >= 500 || (result.object.isEmpty() && response.status >= 200 && response.status < 300));
	return result;
}

void ensure_region_context(CurlSession& session, TikTokStudioAccountCredentials& account)
{
    if (account.region_checked) return;
    account.region_checked = true;
    const auto key = tiktok_region_cache_key(account);
    const auto now = QDateTime::currentSecsSinceEpoch();
    {
        std::lock_guard lock(routing_mutex);
        const auto cached = region_cache.constFind(key);
        if (cached != region_cache.cend() && cached->expires > now) {
            account.priority_region = cached->priority;
            if (account.store_region.isEmpty()) account.store_region = cached->store;
            else region_cache[key].store = account.store_region;
            return;
        }
    }
    for (const auto& host : webcast_hosts(account)) {
        if (tiktok_cancel_requested()) return;
        const auto response = signed_request(session, account, "GET", "https://" + host + "/webcast/game/basic/region_check/", studio_params(account), {},
            "application/x-www-form-urlencoded", {}, 12000);
        if (!response.error.isEmpty()) { if (!response.retryable) return; continue; }
        if (!webcast_error(response.object, "TikTok region check").isEmpty()) return;
        const auto data = response.object.value("data").toObject();
        const auto priority = tiktok_country_code(data.value("priority_region").toString());
        if (priority.isEmpty()) continue;
        account.priority_region = priority;
        if (account.store_region.isEmpty()) account.store_region = tiktok_country_code(data.value("store_region").toString());
        if (account.store_region.isEmpty()) account.store_region = tiktok_country_code(QString::fromUtf8(cookie_value(account.cookie_jar, "store-country-code")));
        std::lock_guard lock(routing_mutex);
        if (region_cache.size() >= 128) region_cache.clear();
        region_cache.insert(key, {account.priority_region, account.store_region, now + 20 * 60});
        return;
    }
    // Discovery is optional, as in the original app. Desktop keeps an empty
    // priority; Android can retain the captured profile's priority instead.
}

QString fetch_studio_version(CurlSession &session)
{
	const Params params{{QStringLiteral("pid"), QStringLiteral("7393277106664249610")},
		{QStringLiteral("uid"), QStringLiteral("0")},
		{QStringLiteral("branch"), QStringLiteral("studio/release/stable")},
		{QStringLiteral("buildId"), QStringLiteral("0")}};
	const Headers headers{{"Accept", "application/json"}};
	const HttpResult response = session.request("GET", request_url(
		QStringLiteral("https://tron-sg.bytelemon.com/api/sdk/check_update"), encode_params(params)),
		headers, {}, studio_user_agent(QString::fromLatin1(minimum_studio_version)), 15000);
	QString error;
	const QJsonObject root = response_object(response, QStringLiteral("LIVE Studio version check"), &error);
	const QString version = root.value(QStringLiteral("data")).toObject()
		.value(QStringLiteral("manifest")).toObject().value(QStringLiteral("win32")).toObject()
		.value(QStringLiteral("version")).toString();
	return version.isEmpty() ? QString::fromLatin1(minimum_studio_version) : normalized_version(version);
}

QString register_device(CurlSession &session, TikTokStudioAccountCredentials *account)
{
	account->live_studio_version = fetch_studio_version(session);
	const QByteArray serial_alphabet("abcdefghijklmnopqrstuvwxyz0123456789");
	const QString pc_serial = random_text(20, serial_alphabet);
	const QString pc_uuid = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-') +
		random_text(16, serial_alphabet);
	QByteArray mac;
	for (int index = 0; index < 6; ++index) {
		int octet = QRandomGenerator::global()->bounded(256);
		if (index == 0)
			octet = (octet | 0x02) & 0xfe;
		if (!mac.isEmpty())
			mac += ':';
		mac += QByteArray::number(octet, 16).rightJustified(2, '0');
	}
	const QStringList models{QStringLiteral("z790 taichi lite"), QStringLiteral("rog strix b760-f"),
		QStringLiteral("b550 aorus elite"), QStringLiteral("prime z690-p"), QStringLiteral("tomahawk x670e")};
	const QStringList os_versions{QStringLiteral("10.0.19045"), QStringLiteral("10.0.22621"),
		QStringLiteral("10.0.22631"), QStringLiteral("10.0.26100"), QStringLiteral("10.0.26200")};
	const QStringList resolutions{QStringLiteral("1920x1080"), QStringLiteral("2560x1440"),
		QStringLiteral("1600x900"), QStringLiteral("1366x768")};
	const QString os_version = os_versions.at(QRandomGenerator::global()->bounded(os_versions.size()));
	const QString model = models.at(QRandomGenerator::global()->bounded(models.size()));
	const QString resolution = resolutions.at(QRandomGenerator::global()->bounded(resolutions.size()));
	const QString width = resolution.section(QLatin1Char('x'), 0, 0);
	const QString height = resolution.section(QLatin1Char('x'), 1, 1);
	const QString locale = QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-'));
	const QString language = QLocale::system().name().section(QLatin1Char('_'), 0, 0).toLower();
	const QString timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
	const int timezone_offset = QDateTime::currentDateTime().offsetFromUtc();
	const int offset_minutes = qAbs(timezone_offset) / 60;
	const QString timezone_gmt = QStringLiteral("GMT%1%2%3")
		.arg(timezone_offset < 0 ? QLatin1Char('-') : QLatin1Char('+'))
		.arg(offset_minutes / 60, 2, 10, QLatin1Char('0'))
		.arg(offset_minutes % 60, 2, 10, QLatin1Char('0'));
	const QJsonObject header{{QStringLiteral("device_id"), 0}, {QStringLiteral("install_id"), 0},
		{QStringLiteral("os"), QStringLiteral("Windows")}, {QStringLiteral("device_platform"), QStringLiteral("PC")},
		{QStringLiteral("sdk_version"), QStringLiteral("1.0.2")}, {QStringLiteral("aid"), QStringLiteral("8311")},
		{QStringLiteral("mc"), QString::fromLatin1(mac)}, {QStringLiteral("channel"), QStringLiteral("studio")},
		{QStringLiteral("package"), QStringLiteral("tiktok_live_studio")},
		{QStringLiteral("language"), locale},
		{QStringLiteral("app_version"), account->live_studio_version},
		{QStringLiteral("os_version"), os_version}, {QStringLiteral("device_model"), model},
		{QStringLiteral("time_zone"), timezone_gmt},
		{QStringLiteral("tz_name"), timezone},
		{QStringLiteral("tz_offset"), timezone_offset}, {QStringLiteral("resolution"), resolution},
		{QStringLiteral("app_region"), QString()}, {QStringLiteral("app_language"), QString()},
		{QStringLiteral("display_name"), QStringLiteral("tiktok_live_studio")},
		{QStringLiteral("pc_uuid"), pc_uuid}, {QStringLiteral("pc_serial"), pc_serial}};
	const QJsonObject payload{{QStringLiteral("header"), header}, {QStringLiteral("_gen_time"), 0},
		{QStringLiteral("magic_tag"), QStringLiteral("ss_app_log")}};
	const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	const QByteArray encrypted = encrypt_tiktok_device_registration_payload(json, QDateTime::currentSecsSinceEpoch());
	if (encrypted.isEmpty())
		return QStringLiteral("The TikTok device-registration payload could not be prepared.");
	const QString browser = studio_browser_version(account->live_studio_version);
	Params params{{QStringLiteral("aid"), QStringLiteral("8311")}, {QStringLiteral("channel"), QStringLiteral("studio")},
		{QStringLiteral("os"), QStringLiteral("Windows")}, {QStringLiteral("os_version"), os_version},
		{QStringLiteral("device_type"), QStringLiteral("PC")}, {QStringLiteral("device_platform"), QStringLiteral("PC")},
		{QStringLiteral("version_code"), account->live_studio_version}, {QStringLiteral("pc_uuid"), pc_uuid},
		{QStringLiteral("pc_serial"), pc_serial}, {QStringLiteral("aid"), QStringLiteral("8311")},
		{QStringLiteral("app_name"), QStringLiteral("tiktok_live_studio")},
		{QStringLiteral("device_id"), QStringLiteral("0")}, {QStringLiteral("install_id"), QStringLiteral("0")},
		{QStringLiteral("channel"), QStringLiteral("studio")},
		{QStringLiteral("version_code"), account->live_studio_version},
		{QStringLiteral("device_platform"), QStringLiteral("windows")},
		{QStringLiteral("timezone_name"), timezone},
		{QStringLiteral("screen_width"), width}, {QStringLiteral("screen_height"), height},
		{QStringLiteral("browser_language"), locale},
		{QStringLiteral("browser_platform"), QStringLiteral("Win32")},
		{QStringLiteral("browser_name"), QStringLiteral("Mozilla")}, {QStringLiteral("browser_version"), browser},
		{QStringLiteral("language"), language}, {QStringLiteral("app_language"), language},
		{QStringLiteral("webcast_language"), language},
		{QStringLiteral("webcast_sdk_version"), QString(account->live_studio_version).remove(QLatin1Char('.'))},
		{QStringLiteral("live_mode"), QStringLiteral("6")}};
	const Headers headers{{"Accept", "application/json, text/plain, */*"}, {"Content-Type", "application/json"},
		{"User-Agent", "TTNetwork PC"}, {"X-SS-DP", ""}, {"sdk_aid", "8311"},
		{"X-SS-Stub", QCryptographicHash::hash(encrypted, QCryptographicHash::Md5).toHex()}};
    const auto log_host = tiktok_resolve_host("log.tiktokv.com", discover_domains(*account));
	const HttpResult response = session.request("POST", request_url(
		QStringLiteral("https://%1/service/2/desktop/device_register/").arg(log_host), encode_params(params)),
		headers, encrypted, "TTNetwork PC", 25000);
	const QByteArray updated_cookies = session.cookies();
	if (!updated_cookies.isEmpty())
		account->cookie_jar = updated_cookies;
	QString error;
	const QJsonObject root = response_object(response, QStringLiteral("TikTok device registration"), &error);
	if (!error.isEmpty())
		return error;
    error = webcast_error(root, "TikTok device registration");
    if (!error.isEmpty()) return error;
	const QJsonObject source = root.value(QStringLiteral("data")).isObject()
		? root.value(QStringLiteral("data")).toObject() : root;
	account->device_id = source.value(QStringLiteral("device_id")).toVariant().toString().trimmed();
	account->install_id = source.value(QStringLiteral("install_id")).toVariant().toString().trimmed();
	if (!account->has_device())
		return QStringLiteral("TikTok device registration did not return device and install identifiers.");
	account->cookie_jar = session.cookies();
	return {};
}

struct LiveSyncResult {
	TikTokStudioLive live;
	QString error;
};

QString split_stream_url(const QString &push_url, QString *server, QString *key)
{
	const int slash = push_url.lastIndexOf(QLatin1Char('/'));
	if (slash <= push_url.indexOf(QStringLiteral("://")) + 2 || slash >= push_url.size() - 1)
		return QStringLiteral("TikTok returned an invalid RTMP push URL.");
	*server = push_url.left(slash);
	*key = push_url.mid(slash + 1);
	return {};
}

QString room_id_from(const QJsonObject &room)
{
	const QJsonObject living = room.value(QStringLiteral("living_room_attrs")).toObject();
	QString value = living.value(QStringLiteral("room_id_str")).toString();
	if (value.isEmpty()) value = living.value(QStringLiteral("room_id")).toVariant().toString();
	if (value.isEmpty()) value = room.value(QStringLiteral("id_str")).toString();
	if (value.isEmpty()) value = room.value(QStringLiteral("id")).toVariant().toString();
	return value;
}

QString stream_id_from(const QJsonObject &room)
{
	QString value = room.value(QStringLiteral("stream_id_str")).toString();
	if (value.isEmpty()) value = room.value(QStringLiteral("stream_id")).toVariant().toString();
	return value;
}

QString heartbeat_sync(CurlSession &session, TikTokStudioAccountCredentials *account,
	const QString &room_id, const QString &stream_id, int status, bool *room_is_living = nullptr)
{
	if (room_is_living)
		*room_is_living = false;
	Params params = studio_params(*account);
	const Params data{{QStringLiteral("status"), QString::number(status)},
		{QStringLiteral("room_id"), room_id}, {QStringLiteral("stream_id"), stream_id}};
	const QByteArray body = encode_params(data);
	QString last_error;
	for (const QString &host : webcast_hosts(*account)) {
		const SignedRequestResult response = signed_request(session, *account, "POST",
			QStringLiteral("https://%1/webcast/room/ping/anchor/").arg(host), params, body,
			"application/x-www-form-urlencoded; charset=UTF-8", {}, 2500);
		const QByteArray updated_cookies = session.cookies();
		if (!updated_cookies.isEmpty())
			account->cookie_jar = updated_cookies;
		if (!response.error.isEmpty()) {
			if (last_error.isEmpty()) last_error = response.error;
            if (!response.retryable) return response.error;
			continue;
		}
		const QString error = webcast_error(response.object, QStringLiteral("TikTok LIVE heartbeat"));
		if (!error.isEmpty()) {
            return error;
		}
		if (room_is_living)
			*room_is_living = response.object.value(QStringLiteral("status_code")).toVariant().toString()
				== QStringLiteral("4003150");
		return {};
	}
	return last_error.isEmpty() ? QStringLiteral("TikTok LIVE heartbeat failed.") : last_error;
}

void pre_finish_sync(CurlSession &session, TikTokStudioAccountCredentials *account,
	const QString &room_id)
{
	const QStringList hosts = webcast_hosts(*account);
	if (hosts.isEmpty())
		return;
	Params params = studio_params(*account);
	params.push_back({QStringLiteral("room_id"), room_id});
	// LIVE Studio sends this advisory request before the two finish pings.
	// Some accounts reject it even though the finish request succeeds.
	signed_request(session, *account, "GET",
		QStringLiteral("https://%1/webcast/room/anchor_pre_finish/").arg(hosts.front()),
		params, {}, "application/x-www-form-urlencoded; charset=UTF-8", {}, 2500);
	const QByteArray updated_cookies = session.cookies();
	if (!updated_cookies.isEmpty())
		account->cookie_jar = updated_cookies;
}

struct ContinuableRoomResult {
	QJsonObject room;
	QString host;
	QString error;
};

void update_session_cookies(CurlSession &session, TikTokStudioAccountCredentials *account)
{
	const QByteArray cookies = session.cookies();
	if (!cookies.isEmpty())
		account->cookie_jar = cookies;
}

ContinuableRoomResult continuable_room_sync(CurlSession &session,
	TikTokStudioAccountCredentials *account)
{
	ContinuableRoomResult result;
	const Params params = studio_params(*account);
	bool confirmed_no_active_room = false;
	for (const QString &host : webcast_hosts(*account)) {
		const SignedRequestResult response = signed_request(session, *account, "GET",
			QStringLiteral("https://%1/webcast/room/continue/").arg(host), params);
		update_session_cookies(session, account);
		if (!response.error.isEmpty()) {
			if (!confirmed_no_active_room && result.error.isEmpty())
				result.error = response.error;
            if (!response.retryable) return result;
			continue;
		}
		const QString response_error = webcast_error(response.object,
			QStringLiteral("Recover TikTok LIVE"));
		if (!response_error.isEmpty()) {
			// /continue/ uses the normal stale-room response when there is no
			// resumable LIVE. That is a successful negative lookup, not a reason
			// to block creation of the next room. Keep checking fallback IDCs in
			// case another region still owns an active continuation.
			if (tiktok_studio_session_is_stale_error(response_error)) {
				confirmed_no_active_room = true;
				result.error.clear();
				continue;
			}
			if (tiktok_studio_session_requires_login(response_error) &&
				!confirmed_no_active_room) {
				result.error = response_error;
				return result;
			}
			if (!confirmed_no_active_room)
				result.error = response_error;
			return result;
		}
		const QJsonObject data = response.object.value(QStringLiteral("data")).toObject();
		const QJsonObject room = data.value(QStringLiteral("room")).toObject();
		if (!room.isEmpty()) {
			result.room = room;
			result.host = host;
			result.error.clear();
			return result;
		}
		confirmed_no_active_room = true;
		result.error.clear();
	}
	if (confirmed_no_active_room)
		result.error.clear();
	return result;
}

LiveSyncResult prepare_live_room_sync(CurlSession &session, TikTokStudioAccountCredentials account,
	const QJsonObject &room_payload, const QString &primary_host, bool send_prepare_heartbeat = true,
	bool require_dual_layout = false)
{
	LiveSyncResult result;
	const QJsonObject room = tiktok_studio_room_object(room_payload);
	const QJsonObject stream_url = room.value(QStringLiteral("stream_url")).toObject();
	const QJsonObject dual_stream_url = room.value(QStringLiteral("multi_stream_url")).toObject();
	update_session_cookies(session, &account);
    account.live_webcast_host = primary_host;
	result.live.account = account;
	result.live.room_id = room_id_from(room);
	result.live.stream_id = stream_id_from(room);
	result.live.owner_user_id = tiktok_studio_room_owner_id(room);
	result.error = split_stream_url(stream_url.value(QStringLiteral("rtmp_push_url")).toString(),
		&result.live.server, &result.live.key);
	const QString dual_push_url = dual_stream_url.value(QStringLiteral("rtmp_push_url")).toString();
	if (!dual_push_url.trimmed().isEmpty()) {
		const QString dual_error = split_stream_url(dual_push_url,
			&result.live.dual_server, &result.live.dual_key);
		if (!dual_error.isEmpty())
			result.error = dual_error;
	}
	if (!result.error.isEmpty() || result.live.room_id.isEmpty() || result.live.stream_id.isEmpty()) {
		if (result.error.isEmpty())
			result.error = QStringLiteral("TikTok did not return room and stream identifiers.");
		return result;
	}
	if (require_dual_layout && (result.live.dual_server.trimmed().isEmpty() ||
		result.live.dual_key.trimmed().isEmpty() ||
        (result.live.server == result.live.dual_server && result.live.key == result.live.dual_key))) {
		result.error = QStringLiteral("TikTok did not return the second stream URL required for Dual Layout.");
		return result;
	}
	if (result.live.owner_user_id.isEmpty())
		result.live.owner_user_id = account.user_id;
	if (result.live.owner_user_id.isEmpty()) {
		Params room_params = studio_params(account);
		room_params.push_back({QStringLiteral("room_id"), result.live.room_id});
		QStringList room_hosts{primary_host};
		room_hosts += webcast_hosts(account);
		room_hosts.removeAll(QString{});
		room_hosts.removeDuplicates();
		for (const QString &room_host : room_hosts) {
			const SignedRequestResult room_info = signed_request(session, account, "GET",
				QStringLiteral("https://%1/webcast/room/info/").arg(room_host), room_params);
			update_session_cookies(session, &account);
			if (!room_info.error.isEmpty() ||
				!webcast_error(room_info.object, QStringLiteral("TikTok room info")).isEmpty())
				continue;
			result.live.owner_user_id = tiktok_studio_room_owner_id(
				room_info.object.value(QStringLiteral("data")).toObject());
			if (!result.live.owner_user_id.isEmpty())
				break;
		}
	}
	if (result.live.owner_user_id.isEmpty()) {
		result.error = QStringLiteral("TikTok did not return the account/anchor ID required for frame signing.");
		result.live.account = account;
		return result;
	}
	account.user_id = result.live.owner_user_id;
	if (send_prepare_heartbeat)
		result.error = heartbeat_sync(session, &account, result.live.room_id, result.live.stream_id, 1);
	result.live.account = account;
	return result;
}

LiveSyncResult continuable_live_sync(TikTokStudioAccountCredentials account, bool resume)
{
	LiveSyncResult result;
	CurlSession session(account.cookie_jar);
	ensure_region_context(session, account);
	ContinuableRoomResult existing = continuable_room_sync(session, &account);
	if (existing.room.isEmpty()) {
		result.live.account = account;
		result.error = existing.error;
		if (resume && result.error.isEmpty())
			result.error = QStringLiteral("TikTok did not return a continuable LIVE session.");
		return result;
	}
	return prepare_live_room_sync(session, account, existing.room, existing.host, resume);
}

LiveSyncResult start_live_sync(TikTokStudioAccountCredentials account, const QString &title,
	const QString &hashtag_id, const QString &game_tag_id, bool mature, bool dual_layout, const TikTokLiveSettings& settings)
{
	LiveSyncResult result;
	CurlSession session(account.cookie_jar);
	ensure_region_context(session, account);
	ContinuableRoomResult existing = continuable_room_sync(session, &account);
	if (!existing.room.isEmpty())
		return prepare_live_room_sync(session, account, existing.room, existing.host, true, dual_layout);
	if (!existing.error.isEmpty()) {
		result.error = existing.error;
		result.live.account = account;
		return result;
	}

	const Params params = studio_params(account);
	Params data{{QStringLiteral("title"), title.trimmed().isEmpty() ? QStringLiteral("Gameplay") : title.trimmed()},
		{QStringLiteral("live_studio"), QStringLiteral("1")}, {QStringLiteral("gen_replay"), QStringLiteral("true")},
		{QStringLiteral("age_restricted"), mature ? QStringLiteral("4") : QStringLiteral("0")},
		{QStringLiteral("cover_uri"), QString()}, {QStringLiteral("close_room_when_close_stream"), QStringLiteral("false")},
		{QStringLiteral("hashtag_id"), hashtag_id.trimmed()},
		{QStringLiteral("game_tag_id"), game_tag_id.trimmed().isEmpty() ? QStringLiteral("0") : game_tag_id.trimmed()},
		{QStringLiteral("game_bitrate_type"), QStringLiteral("high")},
		{QStringLiteral("screenshot_cover_status"), QStringLiteral("1")},
		{QStringLiteral("multi_stream_scene"), dual_layout ? QStringLiteral("1") : QStringLiteral("0")},
		{QStringLiteral("chat_l2"), QStringLiteral("1")}, {QStringLiteral("star_comment_switch"), QStringLiteral("true")},
		{QStringLiteral("multi_stream_source"), QStringLiteral("1")},
		{QStringLiteral("is_group_live_session"), QStringLiteral("false")},
		{QStringLiteral("rtc_net_enabled"), QStringLiteral("false")}};
	if (account.mobile)
		data = mobile_create_form(title, hashtag_id, game_tag_id, account.device_profile, mature, QDateTime::currentMSecsSinceEpoch(), dual_layout, settings);
    else data += tiktok_live_settings_fields(settings, false);
    auto prepare_created = [&](const QJsonObject& room, const QString& host) {
        const auto conflict = tiktok_live_settings_conflict(settings, tiktok_studio_room_object(room));
        auto prepared = prepare_live_room_sync(session, account, room, host, conflict.isEmpty(), dual_layout);
        prepared.live.created_by_us = true;
        // The caller ends this newly created room before any encoder packets
        // can be sent. Missing room_auth metadata is not a contradiction.
        if (!conflict.isEmpty()) prepared.error = conflict;
        return prepared;
    };
	const QByteArray body = encode_params(data);
	QString last_error;
	for (const QString &host : webcast_hosts(account)) {
		if (tiktok_cancel_requested()) break;
		const SignedRequestResult response = signed_request(session, account, "POST",
			QStringLiteral("https://%1/webcast/room/create/").arg(host), params, body);
		update_session_cookies(session, &account);
		QString response_error = response.error;
		if (response_error.isEmpty())
			response_error = webcast_error(response.object, QStringLiteral("Create TikTok LIVE"));
		if (response_error.isEmpty()) {
			const QJsonObject room = tiktok_studio_room_object(
				response.object.value(QStringLiteral("data")).toObject());
            return prepare_created(room, host);
		}

		if (last_error.isEmpty()) last_error = response_error;
        // An explicit TikTok/signing rejection is authoritative. A later
        // fallback host must not hide it behind a generic connection error.
        if (!response.retryable) { last_error = response_error; break; }
		// A POST can reach TikTok even when its response is truncated or the
		// connection closes. Recover the resulting room before another create.
        // A cancelled POST may already have created the room. Complete the
        // read-only recovery so the caller can still clean up that room.
        TikTokRequestCancellation recoverEvenIfCancelled(nullptr);
		ContinuableRoomResult recovered = continuable_room_sync(session, &account);
		if (!recovered.room.isEmpty()) {
            return prepare_created(recovered.room, recovered.host);
        }
		if (tiktok_studio_session_requires_login(response_error) ||
			tiktok_studio_session_requires_login(recovered.error))
			break;
	}
	result.live.account = account;
	result.error = last_error.isEmpty() ? QStringLiteral("TikTok LIVE could not be created.") : last_error;
	return result;
}

} // namespace

QString initialize_tiktok_account(TikTokStudioAccountCredentials &account)
{
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return QStringLiteral("Could not initialize TikTok networking.");
    if (tiktok_session_cookie_header(account.cookie_jar).isEmpty())
        return QStringLiteral("The cookie file contains no valid TikTok cookies.");
    if (account.mobile) {
        try {
            const auto validId = [](const QString& id) {
                return !id.isEmpty() && std::all_of(id.begin(), id.end(), [](QChar c) { return c >= '0' && c <= '9'; }) &&
                    std::any_of(id.begin(), id.end(), [](QChar c) { return c >= '1' && c <= '9'; });
            };
            if ((!account.device_id.isEmpty() || !account.install_id.isEmpty()) &&
                (!validId(account.device_id) || !validId(account.install_id)))
                return "Enter both valid Android IDs, or leave both blank for automatic setup.";
            account.device_profile = complete_mobile_device_profile(account.device_profile);
            account.device_profile["device_id"] = account.device_id;
            account.device_profile["install_id"] = account.install_id;
            if (account.has_device()) return {};
            const auto request = prepare_mobile_registration(account.device_profile);
            // Registration is a bootstrap request and does not need account
            // cookies, RapidAPI, or request/frame signatures.
            CurlSession registration;
            QString error;
            const auto response = response_object(registration.request("POST", request.url.toEncoded(), request.headers,
                request.body, mobile_user_agent(account.device_profile), 25000), "Android device setup", &error);
            if (!error.isEmpty()) return error;
            error = webcast_error(response, "Android device setup");
            if (!error.isEmpty()) return error;
            if (tiktok_cancel_requested()) return "TikTok request cancelled.";
            const auto registered = parse_mobile_registration(response);
            for (auto it = registered.begin(); it != registered.end(); ++it) account.device_profile[it.key()] = it.value();
            account.device_id = registered.value("device_id").toString();
            account.install_id = registered.value("install_id").toString();
            return {};
        } catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
    }
    if (account.has_device()) return {};
    CurlSession session(account.cookie_jar);
    return register_device(session, &account);
}

TikTokStudioLive create_tiktok_live(TikTokStudioAccountCredentials account,
    const QString &title, const QString &topic_id, const QString &game_id, bool mature, QString *error, bool dual_layout,
    const TikTokLiveSettings& settings)
{
    if (!settings.Valid()) { *error = "Choose Off, Own brand, Third-party brand, or Both for content disclosure."; return {}; }
    if (dual_layout && account.mobile && account.device_profile.value("broadcast_type").toString("video") != "video") {
        *error = "Dual layout requires Camera LIVE on Android."; return {};
    }
    if (dual_layout) {
        *error = check_tiktok_dual_access(account);
        if (!error->isEmpty()) return {};
    }
    auto result = start_live_sync(std::move(account), title, topic_id, topic_id == "5" ? game_id : QStringLiteral("0"), mature, dual_layout, settings);
    *error = result.error;
    return result.live;
}

TikTokDualAccess lookup_tiktok_dual_access(TikTokStudioAccountCredentials account)
{
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return {std::nullopt, "Could not initialize TikTok networking."};
    if (!account.mobile) {
        if (account.device_id.isEmpty()) account.device_id = "0";
        if (account.install_id.isEmpty()) account.install_id = "0";
    }
    CurlSession session(account.cookie_jar);
    ensure_region_context(session, account);
    auto flag = [](const QJsonValue& v) -> std::optional<bool> {
        if (v.isBool()) return v.toBool();
        if (v.isDouble() && (v.toDouble() == 0 || v.toDouble() == 1)) return v.toDouble() == 1;
        if (v.isString()) {
            const auto value = v.toString().trimmed().toLower();
            if (value == "true" || value == "1") return true;
            if (value == "false" || value == "0") return false;
        }
        return std::nullopt;
    };
    QString firstError;
    for (const auto& host : webcast_hosts(account)) {
        if (tiktok_cancel_requested()) break;
        auto params = studio_params(account);
        params.append(QPair<QString, QString>{"scene", "1"});
        const auto dual = signed_request(session, account, "GET", "https://" + host + "/webcast/game/basic/create_info/", params);
        auto detail = dual.error.isEmpty() ? webcast_error(dual.object, "Dual layout") : dual.error;
        if (!detail.isEmpty()) {
            if (!dual.retryable) return {std::nullopt, detail};
            if (firstError.isEmpty()) firstError = detail;
            continue;
        }
        // The original app's successful_data rejects replies without an
        // explicit success status; a data-shaped error cannot unlock dual.
        if (dual.object.value("status_code").toVariant().toString() != "0") continue;
        const auto d = dual.object.value("data").toObject();
        const auto multi = flag(d.value("allow_multi_stream"));
        if (!multi) continue;
        if (!*multi) return {false, QStringLiteral("Dual layout is not unlocked for this TikTok account.")};
        const auto threshold = signed_request(session, account, "GET", "https://" + host + "/webcast/anchor_tool/threshold/check/", studio_params(account));
        detail = threshold.error.isEmpty() ? webcast_error(threshold.object, "Dual layout") : threshold.error;
        if (!detail.isEmpty()) {
            if (!threshold.retryable) return {std::nullopt, detail};
            if (firstError.isEmpty()) firstError = detail;
            continue;
        }
        const auto t = threshold.object.value("data").toObject();
        const auto allowed = flag(t.value("allowed")), used = flag(t.value("dual_canvas_used"));
        if (threshold.object.value("status_code").toVariant().toString() != "0" || (!allowed && !used)) continue;
        if (*multi && (allowed.value_or(false) || used.value_or(false))) return {true, {}};
        const int days = t.value("days_to_reach").toVariant().toInt();
        if (*multi && days > 0) return {false, QString("Dual layout is locked by the new-user restriction. Go LIVE %1 more time(s) for 25 minutes each on LIVE Studio. Validation may take up to 48 hours.").arg(days)};
        return {false, QStringLiteral("Dual layout is not unlocked for this TikTok account.")};
    }
    return {std::nullopt, firstError.isEmpty() ? "Could not verify dual-layout access. Check your cookies and RapidAPI key, then refresh." : firstError};
}
QString check_tiktok_dual_access(TikTokStudioAccountCredentials account)
{
    const auto result = lookup_tiktok_dual_access(std::move(account));
    return result.available.value_or(false) ? QString() : result.reason;
}

QVector<TikTokStudioGameTag> fetch_tiktok_game_tags(const QByteArray &cookie_jar, QString *error)
{
    error->clear();
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) { *error = "Could not initialize TikTok networking."; return {}; }
    CurlSession session(cookie_jar);
    TikTokStudioAccountCredentials account; account.cookie_jar = cookie_jar;
    const auto hosts = webcast_hosts(account);
    for (const auto &host : hosts) {
        if (tiktok_cancel_requested()) { *error = "TikTok request cancelled."; return {}; }
        const auto response = session.request("GET",
            QStringLiteral("https://%1/webcast/room/hashtag/list/").arg(host).toUtf8(),
            common_headers(minimum_studio_version), {}, studio_user_agent(minimum_studio_version), 15000);
        error->clear();
        const auto payload = response_object(response, "Load TikTok games", error);
        if (!error->isEmpty()) continue;
        *error = webcast_error(payload, "Load TikTok games");
        if (!error->isEmpty()) continue;
        const auto tags = parse_tiktok_studio_game_tags(payload);
        if (!tags.isEmpty()) return tags;
        *error = "TikTok returned an empty game list.";
    }
    return {};
}

QString ping_tiktok_live(TikTokStudioLive &live, int status)
{
    CurlSession session(live.account.cookie_jar);
    return heartbeat_sync(session, &live.account, live.room_id, live.stream_id, status);
}

QString end_tiktok_live(TikTokStudioLive &live)
{
    if (live.room_id.isEmpty()) return {};
    CurlSession session(live.account.cookie_jar);
    pre_finish_sync(session, &live.account, live.room_id);
    const QString first = heartbeat_sync(session, &live.account, live.room_id, live.stream_id, 4);
    const QString second = heartbeat_sync(session, &live.account, live.room_id, live.stream_id, 4);
    if (first.isEmpty() || second.isEmpty() || tiktok_studio_session_is_stale_error(first) ||
        tiktok_studio_session_is_stale_error(second)) {
        live.room_id.clear();
        return {};
    }
    return second;
}

QByteArray read_tiktok_cookie_file(const QString &path, QString *error)
{
    error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        *error = QStringLiteral("The cookie file is unreadable or exceeds 4 MiB.");
        return {};
    }
    const auto bytes = file.readAll().trimmed();
    QByteArray jar;
    if (bytes.startsWith('[') || bytes.startsWith('{')) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        auto cookies = document.isArray() ? document.array() : document.object().value("cookies").toArray();
        if (parseError.error != QJsonParseError::NoError || cookies.isEmpty()) {
            *error = QStringLiteral("Expected a browser cookie JSON array, a cookies object, or a Netscape cookie file.");
            return {};
        }
        for (const auto &value : cookies) {
            const auto cookie = value.toObject();
            const auto domainValue = cookie.value("domain");
            if (!domainValue.isUndefined() && !domainValue.isString()) continue;
            QByteArray domain = domainValue.toString().trimmed().toLower().toUtf8();
            // TiktokStreaming saves Requests cookies with no domain after
            // loading name/value pairs. Scope these to TikTok for curl.
            if (domain.isEmpty()) domain = "tiktok.com";
            if (domain.startsWith('.')) domain.remove(0, 1);
            if (!is_tiktok_session_host(QString::fromUtf8(domain))) continue;
            const auto name = cookie.value("name").toString().toUtf8();
            const auto content = cookie.value("value").toString().toUtf8();
            auto cookiePath = cookie.value("path").toString("/").toUtf8();
            bool safe = true;
            for (const auto &field : {domain, name, content, cookiePath})
                if (field.contains('\n') || field.contains('\r') || field.contains('\t') || field.contains('\0')) safe = false;
            if (!safe) continue;
            // Session import: only TikTok domains are retained. Curl keeps refreshes in memory.
            jar += '.' + domain + "\tTRUE\t" + cookiePath + "\tTRUE\t0\t" + name + '\t' + content + '\n';
        }
    } else {
        for (const auto &line : bytes.split('\n')) {
            auto fields = line.trimmed().split('\t');
            if (fields.size() != 7) continue;
            auto domain = fields[0];
            if (domain.startsWith("#HttpOnly_")) domain.remove(0, 10);
            if (domain.startsWith('.')) domain.remove(0, 1);
            if (is_tiktok_session_host(QString::fromUtf8(domain))) jar += line.trimmed() + '\n';
        }
    }
    const auto header = tiktok_session_cookie_header(jar);
    if (header.isEmpty() || (tiktok_cookie_value(jar, "sessionid").isEmpty() &&
        tiktok_cookie_value(jar, "sessionid_ss").isEmpty() && tiktok_cookie_value(jar, "sid_tt").isEmpty())) {
        *error = QStringLiteral("The cookie file contains no TikTok login session. Export cookies after signing into TikTok.");
        return {};
    }
    return jar;
}

QJsonObject lookup_tiktok_account_identity(TikTokStudioAccountCredentials& account, QString *error)
{
    error->clear();
    CurlSession session(account.cookie_jar);
    if (account.device_id.isEmpty() && !account.mobile) account.device_id = "0";
    const Params params = account.mobile ? studio_params(account) : passport_signed({
        {"device_id", account.device_id}, {"aid", "8311"}, {"account_sdk_source", "web"},
        {"sdk_version", "2.1.9"}, {"verifyFp", "verify_" + account.device_id}});
    const Headers headers{{"Referer", "https://www.tiktok.com/ucenter_web/live_studio/login"}};
    for (const auto &host : api_hosts(account)) {
        if (tiktok_cancel_requested()) return {};
        const auto response = signed_request(session, account, "GET",
            QStringLiteral("https://%1/passport/account/info/v2/").arg(host), params, {},
            "application/x-www-form-urlencoded", headers, 10000);
        if (!response.error.isEmpty()) {
            if (error->isEmpty()) *error = response.error;
            if (!response.retryable) return {};
            continue;
        }
        error->clear();
        *error = webcast_error(response.object, "TikTok account lookup");
        if (!error->isEmpty()) return {};
        const auto data = response.object.value("data").toObject();
        const auto status = data.value("error_code").toVariant().toString();
        if (!status.isEmpty() && status != "0") {
            *error = QStringLiteral("TikTok rejected the account lookup: %1 (TikTok status %2).").arg(tiktok_api_message(response.object), status);
            return {};
        }
        account.store_region = tiktok_country_code(data.value("region").toString());
        ensure_region_context(session, account);
        account.cookie_jar = session.cookies();
        return data;
    }
    if (error->isEmpty()) *error = "TikTok did not return account details.";
    return {};
}

QString lookup_tiktok_username(TikTokStudioAccountCredentials account, QString* error)
{
    const auto data = lookup_tiktok_account_identity(account, error);
    for (const auto key : {"username", "unique_id", "screen_name", "name"}) {
        const auto username = data.value(key).toString().trimmed();
        if (!username.isEmpty()) return username;
    }
    if (error->isEmpty()) *error = "TikTok did not return a username.";
    return {};
}

QJsonObject tiktok_safety_request(TikTokStudioAccountCredentials& account, const QString& endpoint,
    const QJsonObject& fields, bool post, QString* error)
{
    error->clear();
    const QStringList reads{"eco/violation_list/", "perception/violation/info/"};
    const QStringList posts{"eco/appeal_detail/", "eco/check_appeal/", "eco/appeal/", "perception/appeal/"};
    if (!(post ? posts : reads).contains(endpoint)) { *error = "Unsupported LIVE safety request."; return {}; }
    CurlSession session(account.cookie_jar);
    ensure_region_context(session, account);
    auto params = studio_params(account);
    if (endpoint == "eco/violation_list/" && !account.mobile) {
        params = {{"aid", "8311"}};
        if (!account.device_id.isEmpty()) params.push_back({"device_id", account.device_id});
        if (!account.install_id.isEmpty()) params.push_back({"iid", account.install_id});
    }
    if (!post) {
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) params.push_back({it.key(), it.value().toVariant().toString()});
        if (endpoint != "eco/violation_list/") {
            params.removeIf([](const auto& item) { return item.first == "language"; });
            params.push_back({"language", "en"});
        }
    }
    const auto body = post ? QJsonDocument(fields).toJson(QJsonDocument::Compact) : QByteArray();
    const auto hosts = webcast_hosts(account);
    for (const auto& host : hosts) {
        if (tiktok_cancel_requested()) { *error = "TikTok request cancelled."; return {}; }
        const auto response = signed_request(session, account, post ? "POST" : "GET",
            "https://" + host + "/webcast/" + endpoint, params, body,
            post ? "application/json" : "application/x-www-form-urlencoded; charset=UTF-8");
        account.cookie_jar = session.cookies();
        if (response.error.isEmpty()) {
            error->clear();
            account.live_webcast_host = host;
            return response.object;
        }
        *error = response.error;
        if (post || !response.retryable) break;
    }
    if (error->isEmpty()) *error = "TikTok safety service is unavailable.";
    return {};
}

namespace {
Headers qr_headers()
{
    return {{"Accept", "application/json, text/javascript"},
        {"Referer", "https://www.tiktok.com/ucenter_web/live_studio/login"},
        {"TT-Ticket-Guard-Public-Key", "BHTyDu4GfY+Se8QoOlL22ARkOv4aLE5LBCk8eSiJ6K5N5m8Wg3cPeVYWVYNDrJ3hsRmaVCgNl/AKbMW67VBievw="},
        {"TT-Ticket-Guard-Version", "2"}, {"TT-Ticket-Guard-Web-Version", "1"}, {"TT-Ticket-Guard-Iteration-Version", "0"}};
}
Params qr_params(const TikTokStudioAccountCredentials& account, const QString& token = {})
{
    Params params{{"next", "https://www.tiktok.com"}, {"device_id", account.device_id}, {"aid", "8311"},
        {"account_sdk_source", "web"}, {"sdk_version", "2.1.9"}, {"verifyFp", "verify_" + account.device_id}};
    if (!token.isEmpty()) params += {{"token", token}, {"multi_login", "1"}};
    return passport_signed(params);
}
QString qr_status(const QJsonObject& data)
{
    for (const char* key : {"status", "qr_status", "state"}) {
        const auto value = data.value(key).toVariant().toString().trimmed().toLower();
        if (!value.isEmpty()) return value;
    }
    return {};
}
bool has_login_cookie(const QByteArray& jar)
{
    for (const char* key : {"sessionid", "sessionid_ss", "sid_tt"})
        if (!tiktok_cookie_value(jar, key).isEmpty()) return true;
    return false;
}
}

TikTokQrLogin begin_tiktok_qr_login(TikTokStudioAccountCredentials account)
{
    TikTokQrLogin login;
    // QR login uses the LIVE Studio passport flow for both streaming clients.
    // Keep Android's device profile separate when these cookies are imported.
    account.mobile = false;
    account.cookie_jar.clear();
    account.priority_region.clear(); account.store_region.clear(); account.live_webcast_host.clear(); account.region_checked = false;
    login.account = account;
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) { login.error = "Could not initialize TikTok networking."; return login; }
    if (account.rapidapi_key.trimmed().isEmpty()) { login.error = "Enter a RapidAPI key before signing in."; return login; }
    CurlSession session;
    if (!account.has_device()) {
        login.error = register_device(session, &account);
        login.account = account;
        if (!login.error.isEmpty()) return login;
    }
    const auto params = qr_params(account);
    for (const auto& host : api_hosts(account)) {
        if (tiktok_cancel_requested()) break;
        const auto response = signed_request(session, account, "GET", "https://" + host + "/passport/web/get_qrcode/",
            params, {}, "application/x-www-form-urlencoded", qr_headers(), 12000);
        update_session_cookies(session, &account);
        const auto error = response.error.isEmpty() ? webcast_error(response.object, "TikTok QR login") : response.error;
        if (!error.isEmpty()) {
            if (login.error.isEmpty()) login.error = error;
            if (!response.retryable) break;
            continue;
        }
        const auto data = response.object.value("data").toObject();
        const auto token = data.value("token").toString();
        const auto originalUrl = data.value("qrcode_index_url").toString();
        const auto secret = random_text(8, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        const auto boundUrl = bind_tiktok_qr_client_secret(originalUrl, secret);
        QByteArray png;
        // Do not expect a secret if the URL could not actually be bound.
        const bool bound = !boundUrl.isEmpty() && boundUrl != originalUrl;
        if (bound) png = render_tiktok_qr_png(boundUrl);
        const bool rendered = !png.isEmpty();
        if (!rendered) {
            auto encoded = data.value("qrcode").toString().toLatin1();
            if (encoded.startsWith("data:")) encoded = encoded.mid(encoded.indexOf(',') + 1);
            png = QByteArray::fromBase64(encoded);
        }
        if (token.isEmpty() || png.isEmpty()) { login.error = "TikTok did not return a usable QR code."; continue; }
        login.account = account; login.token = token; login.host = host;
        login.client_secret = rendered ? secret : QString(); login.png = png; login.error.clear();
        return login;
    }
    login.account = account;
    if (login.error.isEmpty()) login.error = tiktok_cancel_requested() ? "TikTok login cancelled." : "TikTok QR login could not be started.";
    return login;
}

TikTokQrLogin poll_tiktok_qr_login(TikTokQrLogin login)
{
    login.error.clear();
    login.retryable = false;
    if (tiktok_cancel_requested() || login.state == TikTokQrState::Confirmed || login.state == TikTokQrState::Expired ||
        login.state == TikTokQrState::Refused || login.state == TikTokQrState::Error) return login;
    if (login.token.isEmpty()) { login.error = "Generate a new QR code to sign in."; return login; }
    auto& account = login.account;
    auto hosts = QStringList{login.host}; hosts += api_hosts(account); hosts.removeAll(QString()); hosts.removeDuplicates();
    const auto params = qr_params(account, login.token);
    QJsonObject best;
    int bestRank = -1;
    for (const auto& host : hosts) {
        if (tiktok_cancel_requested()) return login;
        // A wrong-secret response must not contaminate a later IDC's cookie
        // jar, even if that response includes Set-Cookie headers.
        CurlSession session(account.cookie_jar);
        const auto response = signed_request(session, account, "GET", "https://" + host + "/passport/web/check_qrconnect/",
            params, {}, "application/x-www-form-urlencoded", qr_headers(), 12000);
        const auto error = response.error.isEmpty() ? webcast_error(response.object, "TikTok QR login") : response.error;
        if (!error.isEmpty()) {
            if (login.error.isEmpty()) login.error = error;
            login.retryable = login.retryable || response.transient;
            if (!response.retryable) {
                // Changing TikTok regions cannot repair the shared signer.
                // Keep an earlier valid reply, or let the UI retry after a delay.
                if (response.transient) break;
                login.error = error; login.retryable = false;
                return login;
            }
            continue;
        }
        const auto data = response.object.value("data").toObject();
        const auto status = qr_status(data);
        const bool matches = !login.client_secret.isEmpty() && data.value("client_secret").toString() == login.client_secret;
        if (status == "confirmed" && (login.client_secret.isEmpty() || matches)) {
            update_session_cookies(session, &account);
            login.host = host;
            // A confirmation from another IDC can lack our login cookies.
            // Never export it as a successful login until the session exists.
            if (!has_login_cookie(account.cookie_jar)) { login.error = "TikTok confirmed the code but did not return login cookies. Generate a new code and try again."; login.retryable = false; return login; }
            account.user_id = tiktok_studio_account_user_id(data);
            account.username = data.value("username").toString();
            login.state = TikTokQrState::Confirmed; login.error.clear(); login.retryable = false;
            return login;
        }
        // LIVE Studio prefers a response bound to this code's client secret,
        // then scanned, terminal and new states in that order. A stale 'new'
        // response from another region must not hide expiry indefinitely.
        const int rank = (matches ? 10 : 0) + (status == "scanned" ? 4 :
            status == "expired" || status == "timeout" || status == "refuse" || status == "refused" || status == "error" ? 3 :
            status == "new" ? 2 : 0);
        if (!data.isEmpty() && rank > bestRank) { best = data; bestRank = rank; }
    }
    // Do not import cookies from unconfirmed/wrong-secret IDC replies.
    if (!best.isEmpty()) {
        login.error.clear(); login.retryable = false;
        const auto status = qr_status(best);
        login.state = status == "scanned" ? TikTokQrState::Scanned :
            status == "expired" || status == "timeout" ? TikTokQrState::Expired :
            status == "refuse" || status == "refused" ? TikTokQrState::Refused :
            status == "error" ? TikTokQrState::Error : TikTokQrState::Waiting;
    }
    if (best.isEmpty() && login.error.isEmpty()) login.error = "TikTok did not return a QR login status. Generate a new code and try again.";
    return login;
}

QString save_tiktok_login_cookies(const QByteArray& jar, const QString& directory, QString* error, const QString& username)
{
    error->clear();
    QByteArray filtered;
    QJsonArray cookies;
    for (const auto& line : jar.split('\n')) {
        const auto fields = line.split('\t');
        if (fields.size() != 7) continue;
        auto domain = fields[0]; if (domain.startsWith("#HttpOnly_")) domain.remove(0, 10);
        if (!is_tiktok_session_host(QString::fromUtf8(domain.startsWith('.') ? domain.mid(1) : domain))) continue;
        filtered += line + '\n';
        const auto expires = fields[4].toLongLong();
        cookies.append(QJsonObject{{"name", QString::fromUtf8(fields[5])}, {"value", QString::fromUtf8(fields[6])},
            {"domain", QString::fromUtf8(domain)}, {"path", QString::fromUtf8(fields[2])},
            {"secure", fields[3] == "TRUE"}, {"httpOnly", fields[0].startsWith("#HttpOnly_")},
            {"hostOnly", fields[1] != "TRUE"}, {"session", expires <= 0}, {"expires", expires > 0 ? expires : qint64(-1)}});
    }
    if (!has_login_cookie(filtered)) { *error = "TikTok did not return a login session to save."; return {}; }
    if (directory.isEmpty() || !QDir().mkpath(directory)) { *error = "Could not create the TikTok cookie directory."; return {}; }
    auto name = username.trimmed();
    if (name.startsWith('@')) name.remove(0, 1);
    name = name.left(100);
    for (auto& c : name) if (!c.isLetterOrNumber() && c != '.' && c != '_' && c != '-') c = '_';
    if (name.isEmpty()) name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto base = QDir(directory).filePath("cookies_" + name);
    QTemporaryFile file(QDir(directory).filePath(".cookies-XXXXXX.tmp"));
    const auto bytes = QJsonDocument(cookies).toJson(QJsonDocument::Indented);
    if (!file.open()) { *error = "Could not save the TikTok cookie file: " + file.errorString(); return {}; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.flush()) { *error = "Could not save the TikTok cookie file: " + file.errorString(); return {}; }
    file.close();
    auto path = base + ".json";
    // QTemporaryFile::rename refuses to overwrite a destination, including a file
    // created by another login dialog after our existence check.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!QFile::exists(path) && file.rename(path)) { file.setAutoRemove(false); return path; }
        if (!QFile::exists(path)) { *error = "Could not save the TikTok cookie file: " + file.errorString(); return {}; }
        path = base + '_' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
    }
    *error = "Could not choose a unique TikTok cookie filename.";
    return {};
}
