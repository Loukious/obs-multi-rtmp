#include "live_safety.hpp"
#include "live_client.hpp"
#include "domain_routing.hpp"
#include "api_error.hpp"
#include "request_cancellation.hpp"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QUrlQuery>
#include <QMap>

namespace {
struct Failure { QString message; };
QString Id(const QJsonValue& value)
{
    if (value.isBool() || (!value.isString() && !value.isDouble())) return {};
    const auto id = value.toVariant().toString().trimmed();
    if (id.isEmpty() || id.size() > 20) return {};
    bool positive = false;
    for (const auto c : id) {
        if (c < '0' || c > '9') return {};
        positive |= c != '0';
    }
    return positive ? id : QString();
}
QString RecordId(const QJsonObject& record)
{
    const auto punish = record.value("punish_info").toObject();
    auto id = Id(punish.value("punish_record_id_str"));
    return id.isEmpty() ? Id(punish.value("punish_record_id")) : id;
}
QString ViolationId(const QJsonObject& record)
{
    auto id = Id(record.value("violation_id_str"));
    return id.isEmpty() ? Id(record.value("violation_id")) : id;
}
int Status(const QJsonValue& value)
{
    return value.isDouble() && value.toDouble() == value.toInt(-1) ? value.toInt(-1) : -1;
}
QString AppealStatus(int value, bool snapshot = false)
{
    const QStringList statuses = snapshot ? QStringList{"Not appealed", "Under review", "Not approved", "Penalty adjusted",
        "Permission restored", "Approved", "New restriction", "Permanent restriction"} :
        QStringList{"Appeal unavailable", "Not appealed", "Under review", "Approved", "Not approved", "LIVE ended",
        "Penalty expired", "Appeal deadline passed", "Related LIVE ban", "Approved; restriction remains"};
    return value >= 0 && value < statuses.size() ? statuses[value] : QStringLiteral("Status unavailable");
}
QJsonObject Request(TikTokSafetyContext& context, const QString& endpoint, const QJsonObject& fields, bool post = false)
{
    QString error;
    const auto payload = tiktok_safety_request(context.account, endpoint, fields, post, &error);
    if (!error.isEmpty()) throw Failure{error};
    const auto status = payload.value("status_code");
    if (!(Status(status) == 0 || (status.isString() && status.toString() == "0"))) {
        if (status.isUndefined()) throw Failure{"TikTok returned an unrecognized response; success is unconfirmed."};
        throw Failure{QStringLiteral("TikTok rejected the safety request: %1 (status %2).")
            .arg(tiktok_api_message(payload), status.toVariant().toString())};
    }
    if (endpoint == "eco/violation_list/" && !payload.value("data").isObject())
        throw Failure{"TikTok returned an unrecognized violation list."};
    return payload.value("data").toObject();
}
void CheckCookies(TikTokSafetyContext& context)
{
    if (tiktok_cancel_requested()) throw Failure{"TikTok request cancelled."};
    if (context.cookiePath.isEmpty()) return;
    QString error;
    const auto jar = read_tiktok_cookie_file(context.cookiePath, &error);
    if (!error.isEmpty()) throw Failure{error};
    // A popup belongs to its original login even if a cookie file is replaced.
    if (context.cookieIdentity.isEmpty()) context.cookieIdentity = TikTokSafetyCookieIdentity(context.account.cookie_jar);
    if (context.cookieIdentity.isEmpty()) throw Failure{"The original TikTok login is unavailable. Reopen Safety."};
    if (TikTokSafetyCookieIdentity(jar) != context.cookieIdentity) throw Failure{"The cookie login changed. Reopen Safety for the intended account."};
}
void VerifyAccount(TikTokSafetyContext& context)
{
    CheckCookies(context);
    QString error;
    const auto data = lookup_tiktok_account_identity(context.account, &error);
    if (!error.isEmpty()) throw Failure{error};
    auto user = Id(data.value("user_id_str"));
    if (user.isEmpty()) user = Id(data.value("user_id"));
    if (user.isEmpty()) throw Failure{"TikTok did not confirm the signed-in account ID."};
    if (!context.userId.isEmpty() && user != context.userId) throw Failure{"The signed-in account changed. Reopen this violation."};
    context.userId = user;
    auto name = data.value("username").toString();
    if (name.isEmpty()) name = data.value("unique_id").toString();
    if (!name.isEmpty()) context.label = '@' + name;
}
void DesktopForAppeal(TikTokSafetyContext& context)
{
    CheckCookies(context);
    if (!context.account.mobile) return;
    auto& account = context.account;
    account.mobile = false;
    account.device_profile = context.pcProfile;
    account.device_id = context.pcProfile.value("device_id").toString();
    account.install_id = context.pcProfile.value("install_id").toString();
    account.live_studio_version = context.pcProfile.value("live_studio_version").toString("1.27.0");
    account.live_webcast_host.clear(); account.priority_region.clear(); account.region_checked = false;
    const auto error = initialize_tiktok_account(account);
    if (!error.isEmpty()) throw Failure{error};
    context.pcProfile = account.device_profile;
    context.pcProfile.insert("device_id", account.device_id);
    context.pcProfile.insert("install_id", account.install_id);
}
QVector<QJsonObject> Records(TikTokSafetyContext& context, int kind)
{
    const auto data = Request(context, "eco/violation_list/", {{"violation_list_type", QString::number(kind)}});
    const auto list = data.value("records");
    if (!list.isUndefined() && !list.isNull() && !list.isArray()) throw Failure{"TikTok returned an invalid violation list."};
    QVector<QJsonObject> records;
    for (const auto& value : list.toArray()) if (value.isObject()) records.append(value.toObject());
    return records;
}
QString Redact(QString error, const TikTokSafetyContext& context)
{
    if (!context.account.rapidapi_key.isEmpty()) error.replace(context.account.rapidapi_key, "<redacted>");
    return error;
}
QJsonObject FreshRecord(TikTokSafetyContext& context, const QJsonObject& selected)
{
    VerifyAccount(context);
    const auto key = TikTokViolationKey(selected);
    if (key.isEmpty()) throw Failure{"This violation has no usable record identifier."};
    QString error;
    for (int kind : {0, 1}) {
        try {
            for (const auto& record : Records(context, kind)) if (TikTokViolationKey(record) == key) return record;
        } catch (const Failure& failure) { if (error.isEmpty()) error = failure.message; }
    }
    throw Failure{"This violation could not be verified in the selected account. Refresh Safety. " + error};
}
}

QByteArray TikTokSafetyCookieIdentity(const QByteArray& jar)
{
    for (const auto key : {"sessionid", "sessionid_ss", "sid_tt"}) {
        const auto value = tiktok_cookie_value(jar, key);
        if (!value.isEmpty()) return QCryptographicHash::hash(QByteArray(key) + ':' + value, QCryptographicHash::Sha256);
    }
    return {};
}

QString TikTokViolationKey(const QJsonObject& record)
{
    const auto violation = ViolationId(record), punishment = RecordId(record);
    return violation.isEmpty() && punishment.isEmpty() ? QString() : violation + ':' + punishment;
}

QString TikTokViolationSummary(const QJsonObject& record)
{
    const auto punish = record.value("punish_info").toObject();
    const auto title = punish.value("punish_title").toString("LIVE violation");
    auto reason = punish.value("punish_reason").toString();
    if (reason.isEmpty()) reason = punish.value("moderation_info").toString();
    return reason.isEmpty() ? title : title + "\n" + reason;
}

TikTokAppealRoute TikTokViolationRoute(const QJsonObject& record)
{
    auto value = record.value("punish_info").toObject().value("detail_url").toString();
    QMap<QString, QString> query;
    QUrl official;
    for (int depth = 0; depth < 4; ++depth) {
        if (value.size() > 16384 || value.isEmpty()) break;
        bool controls = false;
        for (const auto c : value) controls |= c.unicode() < 32;
        if (controls) break;
        const QUrl url(value, QUrl::StrictMode);
        if (!url.isValid()) break;
        const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
        QMap<QString, QString> local;
        bool conflict = false;
        for (const auto& item : items) {
            if (local.contains(item.first) || (item.first != "url" && query.contains(item.first) && query[item.first] != item.second)) conflict = true;
            local.insert(item.first, item.second);
            if (item.first != "url") query.insert(item.first, item.second);
        }
        if (conflict) break;
        const QStringList wrappers{"webview", "webcast_webview", "webcast_webview_popup", "webcast_lynxview", "webcast_lynxview_popup"};
        if ((url.scheme() == "sslocal" || url.scheme() == "aweme") && wrappers.contains(url.host())) {
            value = local.value("url"); continue;
        }
        bool trusted = false;
        for (const auto domain : {QString("tiktok.com"), QString("tiktokv.com"), QString("tiktokcdn.com")})
            trusted |= url.host() == domain || url.host().endsWith('.' + domain);
        if (url.scheme() == "https" && trusted && url.userInfo().isEmpty() && url.port(443) == 443) official = url;
        break;
    }
    if (official.isEmpty()) query.clear();
    const auto path = official.path();
    if ((path.contains("ban_detail") || path.contains("ban-detail")) && !Id(query.value("snapshot_id")).isEmpty()) {
        const auto id = Id(query.value("snapshot_id"));
        QUrl url("https://livecenter.tiktok.com/treco/ban_detail");
        QUrlQuery params; params.addQueryItem("snapshot_id", id); params.addQueryItem("feature_type", query.value("feature_type")); url.setQuery(params);
        return {"snapshot", id, {}, query.value("feature_type"), url};
    }
    const bool generic = path.contains("punish_detail") || path.contains("punish-detail");
    const auto id = generic ? Id(query.value(query.contains("record_id") ? "record_id" : "punish_id")) : RecordId(record);
    if (!id.isEmpty()) return {"general", id, {}, {}, QUrl("https://livecenter.tiktok.com/punish_detail?record_id=" + id)};
    return {"external", {}, {}, {}, path.endsWith(".js") ? QUrl() : official};
}

QVector<QPair<QString, QString>> TikTokAppealReasons()
{
    return {{"I do not understand the reason", "pm_mt_rulesGuidance_feedback_sheet_understand_reason"},
        {"I think the restriction is a mistake", "pm_mt_rulesGuidance_feedback_sheet_wrong_reason"},
        {"I do not think I violated the Community Guidelines", "pm_mt_rulesGuidance_feedback_sheet_cg_reason"},
        {"I think the penalty is too severe", "pm_mt_rulesGuidance_feedback_sheet_harsh_reason"},
        {"Other", "pm_mt_rulesGuidance_feedback_sheet_other_reason"}};
}

TikTokSafetySnapshot CheckTikTokViolations(TikTokSafetyContext context)
{
    TikTokSafetySnapshot result; result.context = std::move(context);
    try {
        CheckCookies(result.context);
        if (result.context.userId.isEmpty()) VerifyAccount(result.context);
        result.records = Records(result.context, 0); // Type 0 records are authoritative even when counters say zero.
    } catch (const Failure& failure) { result.error = Redact(failure.message, result.context); }
    return result;
}

TikTokAppealState LoadTikTokAppeal(TikTokSafetyContext context, const QJsonObject& selected)
{
    TikTokAppealState state; state.context = std::move(context); state.record = selected;
    try {
        DesktopForAppeal(state.context);
        state.record = FreshRecord(state.context, selected);
        state.route = TikTokViolationRoute(state.record);
        auto& route = state.route;
        state.reason = "Open TikTok's official details for this type of restriction.";
        if (route.kind == "snapshot") {
            state.detail = Request(state.context, "eco/appeal_detail/", {{"snapshot_id", route.id},
                {"feature_type", route.featureType}, {"version_code", 1}}, true);
            auto owner = Id(state.detail.value("user_id_str"));
            if (owner.isEmpty()) owner = Id(state.detail.value("user_id"));
            if (owner != state.context.userId) throw Failure{"The appeal details do not belong to the selected account."};
            state.status = AppealStatus(Status(state.detail.value("status")), true);
            if (state.detail.value("show_appeal_button").isBool() && state.detail.value("show_appeal_button").toBool()) {
                const auto check = Request(state.context, "eco/check_appeal/", {{"snapshot_id", route.id}}, true);
                const auto code = check.value("check_status");
                state.canSubmit = code.isUndefined() || code.isNull() || Status(code) == 0 ? check.value("status").isBool() && check.value("status").toBool() : Status(code) == 1;
            }
            state.reason = state.canSubmit ? QString() : "TikTok is not offering a new appeal for this restriction.";
        } else if (route.kind == "general") {
            const auto listed = RecordId(state.record);
            const auto data = Request(state.context, "perception/violation/info/", {{"punish_id", route.id}});
            if (!data.value("violation_record").isObject()) throw Failure{"TikTok returned unrecognized violation details."};
            state.record = data.value("violation_record").toObject();
            const auto submission = RecordId(state.record), original = ViolationId(selected), detailed = ViolationId(state.record);
            if (submission.isEmpty() || (!listed.isEmpty() && listed != submission) ||
                (listed.isEmpty() && (original.isEmpty() || original != detailed)) ||
                (!original.isEmpty() && !detailed.isEmpty() && original != detailed)) throw Failure{"TikTok returned a different violation. Refresh Safety."};
            route.submissionId = submission;
            route.officialUrl = QUrl("https://livecenter.tiktok.com/punish_detail?record_id=" + submission);
            state.detail = data.value("detail").toObject();
            const auto punish = state.record.value("punish_info").toObject();
            const auto type = Status(state.detail.contains("punish_type_id") ? state.detail.value("punish_type_id") : punish.value("punish_type_id"));
            const bool copyright = type == 45 || type == 99;
            const auto status = Status(punish.value("appeal_status"));
            state.status = AppealStatus(status); state.canSubmit = status == 1 && !copyright;
            state.reason = copyright ? "This copyright restriction requires TikTok's official evidence form." :
                state.canSubmit ? QString() : "TikTok is not offering a new appeal for this violation.";
        } else state.status = AppealStatus(Status(state.record.value("punish_info").toObject().value("appeal_status")));
    } catch (const Failure& failure) { state.canSubmit = false; state.error = Redact(failure.message, state.context); }
    return state;
}

TikTokAppealState SubmitTikTokAppeal(const TikTokAppealState& reviewed, const QString& reason, const QString& explanation)
{
    auto state = LoadTikTokAppeal(reviewed.context, reviewed.record);
    if (!state.error.isEmpty()) return state;
    try {
        if (!(state.route == reviewed.route)) throw Failure{"The appeal route changed. Refresh and review the details before submitting."};
        if (!state.canSubmit) throw Failure{state.reason};
        QString endpoint;
        QJsonObject body;
        if (state.route.kind == "snapshot") { endpoint = "eco/appeal/"; body = {{"snapshot_id", state.route.id}}; }
        else {
            const auto reasons = TikTokAppealReasons();
            bool valid = false; for (const auto& item : reasons) valid |= item.second == reason;
            if (!valid) throw Failure{"Select a reason for your appeal."};
            const auto text = explanation.trimmed();
            if (text.size() > 140) throw Failure{"The appeal explanation can contain at most 140 characters."};
            const bool other = reason == reasons.back().second;
            if (other && text.isEmpty()) throw Failure{"Explain your appeal when selecting Other."};
            endpoint = "perception/appeal/";
            body = {{"punish_id", state.route.submissionId}, {"text_evidence_title", reason},
                {"text_evidence_desc", other ? text : QString()}, {"appeal_req_scene", 4}, {"enter_from", "rg_violation_list"}, {"language", "en"}};
        }
        VerifyAccount(state.context);
        Request(state.context, endpoint, body, true); // Exactly one mutation attempt.
        state.submitted = true; state.canSubmit = false; state.status = "Appeal submitted";
        auto refreshed = LoadTikTokAppeal(state.context, state.record);
        if (refreshed.error.isEmpty()) {
            refreshed.submitted = true; refreshed.canSubmit = false; return refreshed;
        }
        state.reason = "TikTok accepted the appeal, but the status refresh failed: " + refreshed.error;
    } catch (const Failure& failure) { state.error = Redact(failure.message, state.context); state.canSubmit = false; }
    return state;
}
