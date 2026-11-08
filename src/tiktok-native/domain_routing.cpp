#include "domain_routing.hpp"
#include "tiktok_studio_session.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

QByteArray tiktok_cookie_value(const QByteArray& jar, const QByteArray& name)
{
    for (const auto& raw : jar.split('\n')) {
        const auto line = raw.trimmed();
        if (line.startsWith('#') && !line.startsWith("#HttpOnly_")) continue;
        const auto fields = line.split('\t');
        if (fields.size() >= 7 && fields[5] == name) return fields[6];
    }
    return {};
}
QString tiktok_country_code(const QString& value)
{
    const auto country = value.trimmed().toLower();
    if (country.size() != 2) return {};
    for (auto c : country) if (c < 'a' || c > 'z') return {};
    return country;
}
namespace {
QString cookie(const TikTokStudioAccountCredentials& account, const char* name) { return QString::fromUtf8(tiktok_cookie_value(account.cookie_jar, name)).trimmed().toLower(); }
bool european(const TikTokStudioAccountCredentials& account) {
    static const QStringList countries = QStringLiteral("at be bg ch cy cz de dk ee es fi fr gb gr hr hu ie is it li lt lu lv mt nl no pl pt ro se si sk").split(' ');
    return cookie(account, "tt-target-idc").startsWith("eu") || countries.contains(cookie(account, "store-country-code"));
}
QString canonical_host(QString host) {
    host = host.trimmed().toLower();
    static const QRegularExpression hostname(QStringLiteral("^[a-z0-9]+(?:[a-z0-9.-]*[a-z0-9])?$"));
    if (!hostname.match(host).hasMatch() || !is_tiktok_session_host(host)) return {};
    for (const QString& prefix : {QStringLiteral("webcast16-normal-"), QStringLiteral("api16-normal-"), QStringLiteral("webcast16-ws-")})
        if (host.startsWith(prefix + "alisg.")) return prefix + "c-alisg.tiktokv.com";
    return host;
}
QString region_host(const TikTokStudioAccountCredentials& account) {
    if (european(account)) return "webcast16-normal-no1a.tiktokv.eu";
    if (cookie(account, "store-country-code") == "us" || cookie(account, "tt-target-idc").startsWith("useast")) return "webcast16-normal-useast5.tiktokv.us";
    return {};
}
QString resolve(QString host, const TikTokDispatchMaps& maps) {
    host = canonical_host(host);
    QStringList visited{host};
    for (int hop = 0; hop < 8; ++hop) {
        QString next;
        for (const auto& map : maps) if (map.contains(host)) { next = map.value(host); break; }
        if (next.isEmpty() || visited.contains(next)) break;
        visited.push_back(next); host = next;
    }
    return host;
}
}
QString tiktok_authenticated_host(const TikTokStudioAccountCredentials& account, bool api)
{
    auto segment = cookie(account, "store-idc");
    if (segment.isEmpty()) return {};
    if (segment == "alisg") segment = "c-alisg";
    static const QRegularExpression idc(QStringLiteral("^[a-z0-9]+(?:-[a-z0-9]+)*$"));
    if (!idc.match(segment).hasMatch()) return {};
    const QString suffix = segment == "c-alisg" ? "tiktokv.com" : segment.startsWith("useast") ? "tiktokv.us" :
        segment.startsWith("no") || european(account) ? "tiktokv.eu" : "tiktokv.com";
    return canonical_host((api ? "api16-normal-" : "webcast16-normal-") + segment + '.' + suffix);
}
QString tiktok_resolve_host(const QString& host, const TikTokDispatchMaps& maps) { return resolve(host, maps); }
TikTokDispatchMaps tiktok_dispatch_maps(const QJsonObject& response)
{
    QList<QJsonObject> actions;
    for (auto value : response.value("data").toObject().value("ttnet_dispatch_actions").toArray())
        if (value.isObject()) actions.push_back(value.toObject());
    std::stable_sort(actions.begin(), actions.end(), [](const auto& a, const auto& b) { return a.value("act_priority").toVariant().toInt() < b.value("act_priority").toVariant().toInt(); });
    TikTokDispatchMaps result;
    for (const auto& action : actions) {
        QMap<QString, QString> map;
        const auto strategy = action.value("param").toObject().value("strategy_info").toObject();
        for (auto it = strategy.begin(); it != strategy.end(); ++it) {
            if (!it.value().isString()) continue; // Load-balancing candidate arrays are not final IDC rewrites.
            const auto source = canonical_host(it.key()), target = canonical_host(it.value().toString());
            if (!source.isEmpty() && !target.isEmpty()) map.insert(source, target);
        }
        if (!map.isEmpty()) result.push_back(map);
    }
    return result;
}
QStringList tiktok_host_candidates(const TikTokStudioAccountCredentials& account, bool api, const TikTokDispatchMaps& maps)
{
    QStringList result;
    const auto authenticated = tiktok_authenticated_host(account, api);
    if (!authenticated.isEmpty()) result << authenticated;
    if (api) {
        if (authenticated.isEmpty()) result << resolve("api.tiktokv.com", maps);
        for (const char* host : {"api16-normal-c-alisg.tiktokv.com", "api16-normal-no1a.tiktokv.eu", "api16-normal-useast8.tiktokv.us", "api16-normal-useast5.tiktokv.us"}) result << resolve(host, maps);
    } else {
        if (authenticated.isEmpty()) for (const char* host : {"webcast-normal.tiktokv.com", "webcast.tiktokv.com", "webcast16-normal.tiktokv.com"}) result << resolve(host, maps);
        result << region_host(account);
    }
    result.removeAll(QString()); result.removeDuplicates(); return result;
}
TikTokParams tiktok_dispatch_params(const TikTokStudioAccountCredentials& account)
{
    TikTokParams params;
    if (account.mobile) {
        for (const auto& entry : mobile_request_params(account, {}))
            if (QStringList{"aid", "device_id", "iid", "device_platform", "version_code", "version_name", "ttnet_version", "cronet_version"}.contains(entry.first)) params.push_back(entry);
    } else params = {{"aid", "8311"}, {"ttwebview_version", "1130022001"}, {"device_platform", "win"}, {"tnc_src", "6"}, {"version_code", account.live_studio_version}};
    auto device = tiktok_cookie_value(account.cookie_jar, "device_id");
    if (device.isEmpty()) device = tiktok_cookie_value(account.cookie_jar, "ttwid");
    if (!device.isEmpty() && (!account.mobile || account.device_id.isEmpty())) {
        params.removeIf([](const auto& entry) { return entry.first == "device_id"; });
        params.push_back({"device_id", QString::fromUtf8(device)});
    }
    const auto region = cookie(account, "store-country-code");
    if (!region.isEmpty()) params.push_back({"region", region});
    return params;
}
QByteArray tiktok_routing_cache_key(const TikTokStudioAccountCredentials& account)
{
    QByteArray identity = account.mobile ? "mobile" : "studio";
    identity += '\0' + account.device_id.toUtf8() + '\0' + account.install_id.toUtf8();
    identity += '\0' + (account.mobile ? account.device_profile.value("app_version").toString().toUtf8() : account.live_studio_version.toUtf8());
    for (const char* name : {"sessionid", "sessionid_ss", "sid_guard", "sid_tt", "uid_tt", "uid_tt_ss", "store-idc", "store-country-code", "tt-target-idc"}) identity += '\0' + tiktok_cookie_value(account.cookie_jar, name);
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
}
QByteArray tiktok_region_cache_key(const TikTokStudioAccountCredentials& account)
{
    // Account checks can precede device registration. The same login/client
    // must keep its discovered region when the new device IDs are assigned.
    QByteArray identity = account.mobile ? "mobile" : "studio";
    for (const char* name : {"sessionid", "sessionid_ss", "sid_guard", "sid_tt", "uid_tt", "uid_tt_ss"})
        identity += '\0' + tiktok_cookie_value(account.cookie_jar, name);
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
}
