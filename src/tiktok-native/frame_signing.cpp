// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "frame_signing.hpp"
#include "api_error.hpp"
#include "http_fixture.hpp"
#include "rapidapi_quota.hpp"

#include <curl/curl.h>

#include <QDateTime>
#include <QJsonArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace {

constexpr qsizetype maximum_response_bytes = 16 * 1024 * 1024;

struct HttpResult {
	long status = 0;
	QByteArray body;
	QHash<QByteArray, QByteArray> headers;
	QString error;
};

struct ResponseSink {
	QByteArray *body = nullptr;
	bool too_large = false;
};

bool is_rapidapi_host(const QString &host)
{
	const QString normalized = host.trimmed().toLower();
	return normalized == QStringLiteral("rapidapi.com") ||
		normalized.endsWith(QStringLiteral(".rapidapi.com"));
}

size_t append_response(char *data, size_t size, size_t count, void *user_data)
{
	auto *sink = static_cast<ResponseSink *>(user_data);
	if (!sink || !sink->body || (count != 0 && size > std::numeric_limits<size_t>::max() / count))
		return 0;
	const size_t bytes = size * count;
	if (bytes > static_cast<size_t>(maximum_response_bytes - sink->body->size())) {
		sink->too_large = true;
		return 0;
	}
	sink->body->append(data, static_cast<qsizetype>(bytes));
	return bytes;
}

size_t append_header(char *data, size_t size, size_t count, void *user_data)
{
	auto *result = static_cast<HttpResult *>(user_data);
	if (!result || (count != 0 && size > std::numeric_limits<size_t>::max() / count))
		return 0;
	const QByteArray line(data, static_cast<qsizetype>(size * count));
	const int separator = line.indexOf(':');
	if (separator > 0) {
		const QByteArray name = line.left(separator).trimmed().toLower();
		const QByteArray value = line.mid(separator + 1).trimmed();
		if (!name.isEmpty() && value.size() <= 4096)
			result->headers.insert(name, value);
	}
	return size * count;
}

int cancel_transfer(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const auto *cancelled = static_cast<const std::atomic_bool *>(user_data);
	return cancelled && cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}

HttpResult post_json(const HostedSigningServiceConfig &service, const QUrl &url, const QByteArray &body,
	const std::atomic_bool *cancelled)
{
	HttpResult result;
	if (!service.valid() || url.scheme() != QStringLiteral("https") || !is_rapidapi_host(url.host())) {
		result.error = QStringLiteral("The frame-signing service must use a valid RapidAPI HTTPS address and API key.");
		return result;
	}
	if (cancelled && cancelled->load(std::memory_order_relaxed)) {
		result.error = QStringLiteral("The frame-signing request was cancelled.");
		return result;
	}
	static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
#ifdef TIKTOK_NATIVE_TESTING
	const auto mock = fixture_request("POST", url, {{"X-RapidAPI-Key", service.api_key.toUtf8()}}, body);
	result.status = mock.status;
	result.body = mock.body;
	result.headers = mock.headers;
	result.error = mock.error;
	return result;
#endif
	if (initialized != CURLE_OK) {
		result.error = QStringLiteral("libcurl initialization failed for the frame-signing request.");
		return result;
	}
	CURL *handle = curl_easy_init();
	if (!handle) {
		result.error = QStringLiteral("libcurl could not create a frame-signing request.");
		return result;
	}

	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	const QByteArray host = url.host().toUtf8();
	if (is_rapidapi_host(url.host())) {
		headers = curl_slist_append(headers, (QByteArrayLiteral("X-RapidAPI-Key: ") + service.api_key.toUtf8()).constData());
		headers = curl_slist_append(headers, (QByteArrayLiteral("X-RapidAPI-Host: ") + host).constData());
	}
	const QByteArray encoded_url = url.toEncoded();
	curl_easy_setopt(handle, CURLOPT_URL, encoded_url.constData());
	curl_easy_setopt(handle, CURLOPT_USERAGENT, "TikTokLiveObs/1.0");
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 30000L);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(handle, CURLOPT_POST, 1L);
	curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.constData());
	curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_response);
	ResponseSink sink{&result.body};
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, append_header);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, &result);
	curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, cancel_transfer);
	curl_easy_setopt(handle, CURLOPT_XFERINFODATA, cancelled);
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	const CURLcode code = curl_easy_perform(handle);
	if (sink.too_large)
		result.error = QStringLiteral("The frame-signing service response exceeded the 16 MiB safety limit.");
	else if (cancelled && cancelled->load(std::memory_order_relaxed))
		result.error = QStringLiteral("The frame-signing request was cancelled.");
	else if (code != CURLE_OK)
		result.error = QString::fromLatin1(curl_easy_strerror(code));
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(handle);
	return result;
}

FrameSignResult normalized_result(const QJsonObject &item, const FrameSignInput &input)
{
	const QJsonObject value = item.value(QStringLiteral("signResult")).isObject()
		? item.value(QStringLiteral("signResult")).toObject()
		: item;
	return {
		value.value(QStringLiteral("frametype")).toVariant().toString().isEmpty()
			? input.frame_type : value.value(QStringLiteral("frametype")).toVariant().toString(),
		value.value(QStringLiteral("lid")).toVariant().toString().isEmpty()
			? QStringLiteral("1877999593") : value.value(QStringLiteral("lid")).toVariant().toString(),
		value.value(QStringLiteral("signinfo")).toVariant().toString(),
		value.value(QStringLiteral("signvalue")).toVariant().toString(),
		value.value(QStringLiteral("signversion")).toVariant().toString().isEmpty()
			? QStringLiteral("1.0") : value.value(QStringLiteral("signversion")).toVariant().toString(),
	};
}

qint64 normalized_timestamp(const QJsonObject &item)
{
	QJsonValue value = item.value(QStringLiteral("timestamp"));
	if (value.isUndefined() && item.value(QStringLiteral("signResult")).isObject())
		value = item.value(QStringLiteral("signResult")).toObject().value(QStringLiteral("timestamp"));
	bool valid = false;
	const qint64 timestamp = value.toVariant().toString().toLongLong(&valid);
	return valid ? timestamp : 0;
}

QString response_error(const QJsonObject &object)
{
    return tiktok_api_message(object);
}

QByteArray compact_result_json(const FrameSignResult &result)
{
	const QJsonObject value{
		{QStringLiteral("frametype"), result.frame_type},
		{QStringLiteral("lid"), result.license_id},
		{QStringLiteral("signinfo"), result.sign_info},
		{QStringLiteral("signvalue"), result.sign_value},
		{QStringLiteral("signversion"), result.sign_version},
	};
	return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

} // namespace

bool FrameSignResult::valid() const
{
	return !frame_type.isEmpty() && !license_id.isEmpty() && !sign_info.isEmpty() && !sign_value.isEmpty() &&
		!sign_version.isEmpty();
}

QByteArray FrameSignResult::compact_json() const
{
	return compact_result_json(*this);
}

FrameSignBatch FrameSignClient::fetch_batch(const HostedSigningServiceConfig &service, const FrameSignInput &input,
	qint64 start_timestamp_seconds, int duration_seconds, int step_seconds,
	const std::atomic_bool *cancelled)
{
	FrameSignBatch batch;
	if (!service.valid()) {
		batch.error = QStringLiteral("A RapidAPI frame-signing key is required.");
		return batch;
	}
	QUrl endpoint = service.base_url;
	QString path = endpoint.path();
	if (!path.endsWith(QLatin1Char('/')))
		path += QLatin1Char('/');
	path += input.mobile ? QStringLiteral("framesign") : QStringLiteral("framesign/batch");
	endpoint.setPath(path);

	const QJsonObject sign_input{
		{QStringLiteral("aid"), input.aid},
		{QStringLiteral("uid"), input.uid},
		{QStringLiteral("did"), input.device_id},
		{QStringLiteral("roomid"), input.room_id},
		{QStringLiteral("frametype"), input.frame_type},
		{QStringLiteral("timestamp"), QString::number(start_timestamp_seconds)},
	};
	QJsonObject request{
		{QStringLiteral("payload"), sign_input},
		{QStringLiteral("start_timestamp"), start_timestamp_seconds},
		{QStringLiteral("duration_seconds"), std::clamp(duration_seconds, 1, 900)},
		{QStringLiteral("step_seconds"), std::clamp(step_seconds, 1, 30)},
	};
	if (input.mobile)
		request = {{"payload", sign_input}, {"implementation", "mobile"}, {"sdk_context", input.sdk_context}};
	const HttpResult response = post_json(service, endpoint,
		QJsonDocument(request).toJson(QJsonDocument::Compact), cancelled);
	const auto quota = RecordRapidApiQuota(service, response.headers);
	if (!response.error.isEmpty()) {
		batch.quota = quota;
		batch.error = QStringLiteral("The frame-signing service could not be reached: %1").arg(response.error);
		return batch;
	}
	QByteArray response_body = response.body;
	if (input.mobile && response.status >= 200 && response.status < 300) {
		auto single = QJsonDocument::fromJson(response.body).object();
		if (!single.contains("success") || single.value("success").toBool()) {
			single.insert("timestamp", start_timestamp_seconds);
			response_body = QJsonDocument(QJsonArray{single}).toJson(QJsonDocument::Compact);
		}
	}
	batch = parse_batch_response(response_body, response.status, input);
	batch.quota = quota;
	if (!batch.error.isEmpty() && !service.api_key.isEmpty())
		batch.error.replace(service.api_key, QStringLiteral("<redacted>"), Qt::CaseSensitive);
	return batch;
}

FrameSignBatch FrameSignClient::parse_batch_response(const QByteArray &body, long http_status,
	const FrameSignInput &input)
{
	FrameSignBatch batch;
	QJsonParseError parse_error{};
	const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
	if (!document.isObject() && !document.isArray()) {
		batch.error = QStringLiteral("The frame-signing service returned invalid JSON (HTTP %1).").arg(http_status);
		return batch;
	}
	const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
	const bool explicitly_failed = root.contains(QStringLiteral("success")) &&
		root.value(QStringLiteral("success")).isBool() && !root.value(QStringLiteral("success")).toBool();
	if (http_status < 200 || http_status >= 300 || explicitly_failed) {
		const QString detail = response_error(root);
		batch.error = detail.isEmpty()
			? QStringLiteral("The frame-signing service rejected the request (HTTP %1).").arg(http_status)
			: QStringLiteral("The frame-signing service rejected the request (HTTP %1: %2).")
				.arg(http_status).arg(detail);
		return batch;
	}

	QJsonArray items = document.isArray() ? document.array() : QJsonArray{};
	const QJsonObject startup = root.value(QStringLiteral("startup")).toObject();
	if (!startup.isEmpty())
		items.append(startup);
	for (const QString &key : {QStringLiteral("signatures"), QStringLiteral("results"), QStringLiteral("data")}) {
		if (root.value(key).isArray()) {
			for (const QJsonValue &value : root.value(key).toArray())
				items.append(value);
			break;
		}
	}
	for (const QJsonValue &value : items) {
		if (!value.isObject())
			continue;
		const QJsonObject item = value.toObject();
		const qint64 timestamp = normalized_timestamp(item);
		const FrameSignResult result = normalized_result(item, input);
		if (timestamp > 0 && result.valid() && (!input.mobile || result.sign_version == "2.0"))
			batch.signatures.insert(timestamp, result);
	}
	if (batch.signatures.isEmpty())
		batch.error = QStringLiteral("The frame-signing service returned no usable signatures.");
	return batch;
}
