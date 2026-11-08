// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_studio_session.hpp"

#include <QJsonValue>
#include <QMap>

#include <initializer_list>

namespace {

constexpr qsizetype maximum_cookie_header_bytes = 64 * 1024;

bool exact_or_subdomain(const QString &host, const QString &domain)
{
	return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

bool safe_cookie_name(const QByteArray &name)
{
	if (name.isEmpty())
		return false;
	static const QByteArray separators("()<>@,;:\\\"/[]?={}");
	for (const unsigned char byte : name) {
		if (byte <= 0x20 || byte >= 0x7f || separators.contains(static_cast<char>(byte)))
			return false;
	}
	return true;
}

bool safe_cookie_value(const QByteArray &value)
{
	return !value.contains('\r') && !value.contains('\n') && !value.contains('\0') &&
		!value.contains('\t');
}

QString identifier_value(const QJsonValue &value)
{
	if (value.isString())
		return value.toString().trimmed();
	if (!value.isDouble())
		return {};
	// Qt 6 preserves JSON integers as 64-bit values. Converting through
	// double rounds TikTok account IDs and breaks later identity checks.
	const qint64 number = value.toInteger(-1);
	if (number < 0)
		return {};
	return QString::number(number);
}

QString identifier_from(const QJsonObject &object, std::initializer_list<const char *> keys)
{
	for (const char *key : keys) {
		const QString value = identifier_value(object.value(QString::fromLatin1(key)));
		if (!value.isEmpty() && value != QStringLiteral("0"))
			return value;
	}
	return {};
}

QString user_id_from_object(const QJsonObject &object)
{
	return identifier_from(object, {"user_id_str", "user_id", "uid_str", "uid", "id_str", "id"});
}

} // namespace

bool is_tiktok_session_host(const QString &host)
{
	const QString normalized = host.trimmed().toLower();
	return exact_or_subdomain(normalized, QStringLiteral("tiktok.com")) ||
		exact_or_subdomain(normalized, QStringLiteral("tiktokv.com")) ||
		exact_or_subdomain(normalized, QStringLiteral("tiktokv.us")) ||
		exact_or_subdomain(normalized, QStringLiteral("tiktokv.eu"));
}

QByteArray tiktok_session_cookie_header(const QByteArray &netscape_cookie_jar)
{
	QMap<QByteArray, QByteArray> cookies;
	for (const QByteArray &raw_line : netscape_cookie_jar.split('\n')) {
		const QByteArray line = raw_line.trimmed();
		if (line.isEmpty() || (line.startsWith('#') && !line.startsWith("#HttpOnly_")))
			continue;
		const QList<QByteArray> fields = line.split('\t');
		if (fields.size() < 7)
			continue;
		const QByteArray name = fields.at(5).trimmed();
		const QByteArray value = fields.at(6);
		if (safe_cookie_name(name) && safe_cookie_value(value))
			cookies.insert(name, value);
	}

	QByteArray header;
	for (auto it = cookies.cbegin(); it != cookies.cend(); ++it) {
		const QByteArray item = it.key() + '=' + it.value();
		const qsizetype separator = header.isEmpty() ? 0 : 2;
		if (header.size() + separator + item.size() > maximum_cookie_header_bytes)
			return {};
		if (!header.isEmpty())
			header += "; ";
		header += item;
	}
	return header;
}

bool tiktok_studio_session_is_stale_error(const QString &error)
{
	const QString normalized = error.trimmed().toLower();
	// TikTok's /room/continue/ endpoint uses both 30001 (the room does not
	// exist) and 30003 (the previous LIVE ended) to signal that no resumable
	// session is available. Both are normal negative lookup results.
	return normalized.contains(QStringLiteral("30001")) ||
		normalized.contains(QStringLiteral("30003")) ||
		normalized.contains(QStringLiteral("live room does not exist")) ||
		normalized.contains(QStringLiteral("live has ended")) ||
		normalized.contains(QStringLiteral("room has finished"));
}

bool tiktok_studio_session_requires_login(const QString &error)
{
	const QString normalized = error.trimmed().toLower();
	return normalized.contains(QStringLiteral("please login")) ||
		normalized.contains(QStringLiteral("login first")) ||
		normalized.contains(QStringLiteral("login required")) ||
		normalized.contains(QStringLiteral("not logged in")) ||
		normalized.contains(QStringLiteral("sign in first"));
}

bool tiktok_studio_session_has_no_live_auth(const QString &error)
{
	const QString normalized = error.trimmed().toLower();
	return normalized.contains(QStringLiteral("20800")) ||
		normalized.contains(QStringLiteral("no live auth"));
}

QString tiktok_studio_account_user_id(const QJsonObject &account_data)
{
	QString value = user_id_from_object(account_data);
	if (!value.isEmpty())
		return value;
	for (const char *key : {"user", "account", "user_info", "passport_user", "profile"}) {
		value = user_id_from_object(account_data.value(QString::fromLatin1(key)).toObject());
		if (!value.isEmpty())
			return value;
	}
	return {};
}

QJsonObject tiktok_studio_room_object(const QJsonObject &data)
{
	const QJsonObject room = data.value(QStringLiteral("room")).toObject();
	return room.isEmpty() ? data : room;
}

QString tiktok_studio_room_owner_id(const QJsonObject &room_data)
{
	const QJsonObject room = tiktok_studio_room_object(room_data);
	QString value = identifier_from(room,
		{"owner_user_id_str", "owner_user_id", "anchor_id_str", "anchor_id"});
	if (!value.isEmpty())
		return value;

	const QJsonObject living = room.value(QStringLiteral("living_room_attrs")).toObject();
	value = identifier_from(living,
		{"owner_user_id_str", "owner_user_id", "anchor_id_str", "anchor_id"});
	if (!value.isEmpty())
		return value;

	for (const char *key : {"owner", "anchor", "user"}) {
		value = tiktok_studio_account_user_id(room.value(QString::fromLatin1(key)).toObject());
		if (!value.isEmpty())
			return value;
	}
	return {};
}
