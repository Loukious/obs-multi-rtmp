#pragma once
#include "tiktok_studio_account.hpp"
#include "mobile_profile.hpp"
#include <QMap>
#include <QStringList>

using TikTokDispatchMaps = QList<QMap<QString, QString>>;
QByteArray tiktok_cookie_value(const QByteArray& jar, const QByteArray& name);
QString tiktok_country_code(const QString& value);
QString tiktok_authenticated_host(const TikTokStudioAccountCredentials& account, bool api);
TikTokDispatchMaps tiktok_dispatch_maps(const QJsonObject& response);
QString tiktok_resolve_host(const QString& host, const TikTokDispatchMaps& maps);
QStringList tiktok_host_candidates(const TikTokStudioAccountCredentials& account, bool api, const TikTokDispatchMaps& maps);
TikTokParams tiktok_dispatch_params(const TikTokStudioAccountCredentials& account);
QByteArray tiktok_routing_cache_key(const TikTokStudioAccountCredentials& account);
QByteArray tiktok_region_cache_key(const TikTokStudioAccountCredentials& account);
