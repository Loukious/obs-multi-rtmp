// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_request_signer.hpp"

#include "localization.hpp"
#include "request_cancellation.hpp"
#include "http_fixture.hpp"
#include "http_retry.hpp"
#include "rapidapi_quota.hpp"
#include "api_error.hpp"

#include <curl/curl.h>

#include <QDateTime>
#include <QJsonDocument>
#include <QHash>
#include <QJsonObject>
#include <QUrl>

#include <initializer_list>
#include <limits>

namespace {

constexpr qsizetype response_limit = 1024 * 1024;

struct HttpResult {
	long status = 0;
	QByteArray body;
	QHash<QByteArray, QByteArray> headers;
	QString error;
	bool overflow = false;
};

size_t append_header(char *data, size_t size, size_t count, void *user_data)
{
	auto *result = static_cast<HttpResult *>(user_data);
	if (!result || (count != 0 && size > std::numeric_limits<size_t>::max() / count))
		return 0;
	const size_t bytes = size * count;
	const QByteArray line(data, static_cast<qsizetype>(bytes));
	const int separator = line.indexOf(':');
	if (separator > 0) {
		const QByteArray name = line.left(separator).trimmed().toLower();
		const QByteArray value = line.mid(separator + 1).trimmed();
		if (!name.isEmpty() && value.size() <= 4096)
			result->headers.insert(name, value);
	}
	return bytes;
}

size_t append_response(char *data, size_t size, size_t count, void *user_data)
{
	auto *result = static_cast<HttpResult *>(user_data);
	const size_t bytes = size * count;
	if (bytes > static_cast<size_t>(response_limit) ||
		result->body.size() > response_limit - static_cast<qsizetype>(bytes)) {
		result->overflow = true;
		return 0;
	}
	result->body.append(data, static_cast<qsizetype>(bytes));
	return bytes;
}

QString response_message(const QJsonObject &object)
{
    return tiktok_api_message(object);
}

QString rapidapi_rejection_message(long http_status, const QString &detail)
{
	const QString normalized = detail.trimmed().toCaseFolded();
	// RapidAPI normally reports a missing, malformed, or revoked key as 401.
	// Some gateways use 403 for the same key problem, while others use it for a
	// missing subscription. Prefer an explicit response message over status.
	const bool mentions_key = normalized.contains(QStringLiteral("api key")) ||
		normalized.contains(QStringLiteral("x-rapidapi-key")) ||
		normalized.contains(QStringLiteral("unauthorized")) ||
		normalized.contains(QStringLiteral("invalid key"));
	const bool mentions_subscription = normalized.contains(QStringLiteral("subscribe")) ||
		normalized.contains(QStringLiteral("subscription")) ||
		normalized.contains(QStringLiteral("not subscribed"));

	if (http_status == 401 || (http_status == 403 && mentions_key)) {
		return translated_or("Provider.TikTokStudio.RapidApi.InvalidKey", QStringLiteral(
			"RapidAPI rejected your API key. Check that you copied the full active key and try again."));
	}
	if (http_status == 403 || mentions_subscription) {
		return translated_or("Provider.TikTokStudio.RapidApi.Subscription", QStringLiteral(
			"RapidAPI denied access to this signer. Check your API key and confirm that its subscription is active."));
	}
	if (http_status == 429 || normalized.contains(QStringLiteral("too many request")) ||
		normalized.contains(QStringLiteral("rate limit"))) {
		return translated_or("Provider.TikTokStudio.RapidApi.RateLimited", QStringLiteral(
			"RapidAPI reported a request limit (HTTP 429). This does not automatically mean that your API key is invalid. Check your RapidAPI dashboard for usage, subscription, and that your API key was entered correctly."));
	}
	return detail.isEmpty()
		? QStringLiteral("RapidAPI rejected the request-signing request (HTTP %1).").arg(http_status)
		: QStringLiteral("RapidAPI rejected the request-signing request: %1").arg(detail);
}

QByteArray header_value(const QJsonObject &object, const QString &name)
{
	QJsonValue value = object.value(name);
	if (value.isUndefined())
		value = object.value(name.toUpper());
	return value.toVariant().toString().trimmed().toUtf8();
}

} // namespace

TikTokRequestSignatureHeaders RapidApiRequestSigner::fetch(const HostedSigningServiceConfig &service,
	const TikTokRequestSignatureInput &input)
{
	TikTokRequestSignatureHeaders signatures;
	if (tiktok_cancel_requested()) { signatures.error = "TikTok request cancelled."; return signatures; }
	if (!service.valid()) {
		signatures.error = QStringLiteral("A valid RapidAPI signer key and HTTPS endpoint are required.");
		return signatures;
	}
	if (input.timestamp_seconds <= 0 || input.encoded_query.size() > 128 * 1024 ||
		input.body_stub.size() > 256) {
		signatures.error = QStringLiteral("The TikTok request-signing input is invalid.");
		return signatures;
	}

	QUrl endpoint = service.base_url;
	QString path = endpoint.path();
	if (!path.endsWith(QLatin1Char('/')))
		path += QLatin1Char('/');
	endpoint.setPath(path + QStringLiteral("signatures"));
	QJsonObject payload{
		{QStringLiteral("timestamp"), input.timestamp_seconds},
		{QStringLiteral("aid"), input.aid},
		{QStringLiteral("device_id"), input.device_id},
		{QStringLiteral("license_id"), 1877999593},
		{QStringLiteral("params"), QString::fromUtf8(input.encoded_query)},
		{QStringLiteral("stub"), QString::fromLatin1(input.body_stub)},
	};
	if (input.mobile) {
		payload.insert("implementation", "mobile");
		payload.insert("url", input.url);
		payload.insert("headers", input.headers);
		payload.insert("body_base64", QString::fromLatin1(input.body.toBase64()));
		payload.insert("install_id", input.install_id);
	}
	const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

#ifdef TIKTOK_NATIVE_TESTING
	const auto mock = fixture_request("POST", endpoint, {{"X-RapidAPI-Key", service.api_key.toUtf8()}}, body);
	signatures = parse_response(mock.body, mock.status);
	signatures.quota = RecordRapidApiQuota(service, mock.headers);
	if (mock.curl_code != CURLE_OK || !mock.error.isEmpty()) {
		signatures.error = QStringLiteral("The RapidAPI request signer could not be reached: %1")
			.arg(mock.error.isEmpty() ? QString::fromLatin1(curl_easy_strerror(mock.curl_code)) : mock.error);
		signatures.transient = tiktok_transient_transport_error(mock.curl_code);
	}
	if (input.mobile && signatures.error.isEmpty() && signatures.x_gorgon.isEmpty())
        signatures.error = "RapidAPI did not return the X-Gorgon signature required for Android.";
	return signatures;
#endif

	HttpResult result;
	CURL *handle = curl_easy_init();
	if (!handle) {
		signatures.error = QStringLiteral("libcurl could not create a RapidAPI request.");
		return signatures;
	}
	curl_slist *headers = nullptr;
	const QByteArray host = endpoint.host().toUtf8();
	const QByteArray rapidapi_key = service.api_key.toUtf8();
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, (QByteArray("X-RapidAPI-Key: ") + rapidapi_key).constData());
	headers = curl_slist_append(headers, (QByteArray("X-RapidAPI-Host: ") + host).constData());
	const QByteArray encoded_url = endpoint.toEncoded();
	curl_easy_setopt(handle, CURLOPT_URL, encoded_url.constData());
	curl_easy_setopt(handle, CURLOPT_POST, 1L);
	curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.constData());
	curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, "TikTokLiveOBS/0.1");
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 20000L);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, append_header);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, &result);
	set_tiktok_request_cancellation(handle);
	const CURLcode code = curl_easy_perform(handle);
	if (code != CURLE_OK)
		result.error = result.overflow ? QStringLiteral("RapidAPI returned an oversized response.")
			: QString::fromLatin1(curl_easy_strerror(code));
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(handle);
	const auto quota = RecordRapidApiQuota(service, result.headers);
	if (!result.error.isEmpty()) {
		signatures.quota = quota;
		signatures.transient = tiktok_transient_transport_error(code);
		signatures.error = QStringLiteral("The RapidAPI request signer could not be reached: %1")
			.arg(result.error);
		return signatures;
	}
	signatures = parse_response(result.body, result.status);
	if (input.mobile && signatures.error.isEmpty() && signatures.x_gorgon.isEmpty())
		signatures.error = QStringLiteral("RapidAPI did not return the X-Gorgon signature required for Android.");
	signatures.quota = quota;
	if (!signatures.error.isEmpty())
		signatures.error.replace(service.api_key, QStringLiteral("<redacted>"), Qt::CaseSensitive);
	return signatures;
}

TikTokRequestSignatureHeaders RapidApiRequestSigner::parse_response(const QByteArray &body,
	long http_status)
{
	TikTokRequestSignatureHeaders signatures;
	signatures.transient = tiktok_transient_http_status(http_status);
	const QJsonDocument document = QJsonDocument::fromJson(body);
	if (!document.isObject()) {
		signatures.error = QStringLiteral("RapidAPI returned an invalid request-signing response.");
		return signatures;
	}
	const QJsonObject root = document.object();
	const QJsonValue success = root.value(QStringLiteral("success"));
	if (http_status < 200 || http_status >= 300 ||
		(!success.isUndefined() && !success.toBool())) {
		const QString detail = response_message(root);
		signatures.error = rapidapi_rejection_message(http_status, detail);
        if (!detail.isEmpty() && !signatures.error.contains(detail)) signatures.error += "\n" + detail;
		return signatures;
	}
	const QJsonObject headers = root.value(QStringLiteral("headers")).isObject()
		? root.value(QStringLiteral("headers")).toObject() : root;
	signatures.x_khronos = header_value(headers, QStringLiteral("x-khronos"));
	signatures.x_ladon = header_value(headers, QStringLiteral("x-ladon"));
	signatures.x_argus = header_value(headers, QStringLiteral("x-argus"));
	signatures.x_gorgon = header_value(headers, QStringLiteral("x-gorgon"));
	if (!signatures.valid())
		signatures.error = QStringLiteral("RapidAPI returned incomplete TikTok request signatures.");
	return signatures;
}
