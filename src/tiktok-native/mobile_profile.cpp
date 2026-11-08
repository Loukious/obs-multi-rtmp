#include "mobile_profile.hpp"
#include <QMap>

TikTokParams mobile_request_params(const TikTokStudioAccountCredentials &account, const QString &country)
{
    QMap<QString, QString> values;
    const auto profile = account.device_profile;
    const auto extras = profile.value("request_params").toObject();
    for (auto it = extras.begin(); it != extras.end(); ++it)
        values[it.key()] = it.value().toVariant().toString();
    const auto fingerprint = profile.value("fingerprint").toObject();
    const auto identifiers = profile.value("identifiers").toObject();
    const QList<QPair<QString, QString>> names{{"model", "device_type"}, {"brand", "device_brand"},
        {"os_version", "os_version"}, {"os_api", "os_api"}, {"resolution", "resolution"},
        {"dpi", "dpi"}, {"timezone_name", "timezone_name"}, {"region", "region"},
        {"android_id", "android_id"}, {"clientudid", "clientudid"}};
    for (const auto &[source, dest] : names) {
        auto value = fingerprint.value(source).toVariant().toString();
        if (value.isEmpty()) value = identifiers.value(source).toVariant().toString();
        if (!value.isEmpty()) values[dest] = value;
    }
    for (const auto key : {"install_id", "live_studio", "live_mode", "browser_name", "browser_version",
        "browser_platform", "browser_language", "screen_width", "screen_height", "multi_stream_scene", "multi_stream_source"})
        values.remove(key);
    const QString version = profile.value("app_version").toString("47.1.4");
    int code = 0;
    for (const auto &part : version.split('.')) code = code * 100 + part.toInt();
    values["aid"] = "1233";
    values["device_id"] = account.device_id;
    values["iid"] = account.install_id;
    values["device_platform"] = "android";
    values["app_name"] = "musical_ly";
    values["channel"] = "googleplay";
    values["version_name"] = version;
    values["version_code"] = QString::number(code);
    values["manifest_version_code"] = profile.value("manifest_version_code").toString(profile.value("version_code").toString("2024701040"));
    values["update_version_code"] = profile.value("update_version_code").toString(values["manifest_version_code"]);
    values["ttnet_version"] = profile.value("ttnet_version").toString("4.2.243.64-tiktok");
    values["cronet_version"] = profile.value("cronet_version").toString("eeeffe19_2026-08-31");
    values["language"] = values["app_language"] = values["webcast_language"] = "en";
    if (!country.isEmpty()) values["priority_region"] = country;
    else values.remove("priority_region");
    TikTokParams result;
    for (auto it = values.begin(); it != values.end(); ++it) result.append({it.key(), it.value()});
    return result;
}

TikTokParams mobile_create_form(const QString &title, const QString &topic_id, const QString &game_id,
    const QJsonObject &profile, bool mature, qint64 timestamp_ms, bool dual_layout, const TikTokLiveSettings& settings)
{
    TikTokParams form{{"title", title.trimmed().isEmpty() ? "LIVE" : title.trimmed()}, {"cover_uri", ""},
        {"hashtag_id", topic_id}, {"gen_replay", "true"},
        {"age_restricted", mature ? "4" : "0"}, {"live_agreement", "0"}, {"game", ""}, {"tags", ""},
        {"has_commerce_goods", "false"}, {"deco_list", ""}, {"hold_living_room", "1"},
        {"create_source", "0"}, {"chat_l_2", "1"},
        {"timestamp", QString::number(timestamp_ms)}};
    if (profile.value("broadcast_type").toString("video") == "screen_record") {
        form.append(QPair<QString, QString>{"screen_shot", "1"});
        form.append(QPair<QString, QString>{"live_room_mode", "4"});
        form.append({"orientation", profile.value("orientation").toVariant().toString()});
        form.append({"game_tag_id", topic_id == "5" && !game_id.isEmpty() ? game_id : QStringLiteral("0")});
    }
    if (dual_layout && profile.value("broadcast_type").toString("video") == "video") {
        form.append(QPair<QString, QString>{"live_studio", "1"});
        form.append(QPair<QString, QString>{"multi_stream_scene", "1"});
        form.append(QPair<QString, QString>{"multi_stream_source", "1"});
    }
    form += tiktok_live_settings_fields(settings, true);
    return form;
}

QJsonObject mobile_frame_context(const QJsonObject &profile)
{
    const auto saved = profile.value("frame_sdk").toObject();
    return saved.isEmpty() ? QJsonObject{{"sdk_version", "v05.03.01-ov-android"}, {"rk_k", 0},
        {"metadata_field6", 270088}, {"metadata_field7", 4}, {"captured_rule_1008", false}} : saved;
}

QByteArray mobile_user_agent(const QJsonObject &profile)
{
    return profile.value("user_agent").toString("TikTok/" + profile.value("app_version").toString("47.1.4") + " (Android)").toUtf8();
}
