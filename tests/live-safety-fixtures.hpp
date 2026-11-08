#pragma once
#include "live_safety.hpp"
#include "tiktok_studio_session.hpp"
#include <QUrlQuery>

inline QJsonObject SafetyRecord()
{
    return {{"violation_id_str", "100"}, {"live_info", QJsonObject{{"title", "Fixture LIVE"}}},
        {"punish_info", QJsonObject{{"punish_record_id_str", "200"}, {"punish_title", "LIVE visibility restricted"},
        {"punish_reason", "Community Guidelines"}, {"appeal_status", 1},
        {"detail_url", "https://livecenter.tiktok.com/punish_detail?record_id=200"}}}};
}

inline void LiveSafetyFixtures()
{
    TikTokSafetyContext context; context.userId = "123"; context.label = "fixture";
    auto& account = context.account;
    account.device_id = "11"; account.install_id = "12"; account.rapidapi_key = "safety-fixture-key";
    account.cookie_jar = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tsafety-fixture\n.tiktok.com\tTRUE\t/\tTRUE\t0\tstore-idc\tuseast1a\n";
    account.region_checked = true; account.live_webcast_host = "webcast16-normal-c-useast1a.tiktokv.com";
    auto record = SafetyRecord();
    QString user = "123", detailUser = "123";
    int mutations = 0, listCalls = 0, detailCalls = 0, type = 29;
    QJsonValue eligibility = 1;
    bool listed = true, button = true, timeout = false, malformed = false, reject = false, refreshFailure = false, wrongDetail = false;
    bool failFirstList = false;
    bool numericAccount = false;
    QString mutationPath, lookupId;
    QJsonObject mutationBody, signature;
    tiktok_http_fixture = [&](const QByteArray& method, const QUrl& url, const TikTokHttpHeaders& headers, const QByteArray& body) -> TikTokHttpFixtureResponse {
        const auto path = url.path();
        const auto query = QUrlQuery(url);
        if (path == "/signatures") { signature = QJsonDocument::fromJson(body).object(); return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"}; }
        if (path == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"us"}})"};
        if (path == "/passport/account/info/v2/") return {200, Json({{"status_code", 0}, {"data", QJsonObject{
            {numericAccount ? "user_id" : "user_id_str", numericAccount ? QJsonValue(user.toLongLong()) : QJsonValue(user)}, {"username", "fixture"}}}})};
        if (path == "/webcast/eco/violation_list/") {
            ++listCalls;
            if (failFirstList) { failFirstList = false; return {503, R"({"message":"fixture host unavailable"})"}; }
            Check(method == "GET" && (query.queryItemValue("violation_list_type") == "0" || query.queryItemValue("violation_list_type") == "1"), "Violation list type sent in signed GET query");
            Check(query.queryItemValue("aid") == (signature.value("implementation") == "mobile" ? "1233" : "8311"), "Violation list uses the selected desktop/mobile request identity");
            return {200, Json({{"status_code", 0}, {"data", QJsonObject{{"active_count", 0}, {"records", listed ? QJsonArray{record} : QJsonArray{}}}}})};
        }
        if (path == "/webcast/perception/violation/info/") {
            ++detailCalls; lookupId = query.queryItemValue("punish_id");
            Check(query.allQueryItemValues("language") == QStringList{"en"}, "Appeal details override the device locale with one official form language parameter");
            if (refreshFailure && mutations) return {500, R"({"message":"refresh failed"})"};
            auto detailRecord = record;
            if (wrongDetail) { auto punish = detailRecord["punish_info"].toObject(); punish["punish_record_id_str"] = "999"; detailRecord["punish_info"] = punish; }
            return {200, Json({{"status_code", 0}, {"data", QJsonObject{{"violation_record", detailRecord}, {"detail", QJsonObject{{"punish_type_id", type}}}}}})};
        }
        if (path == "/webcast/eco/appeal_detail/") {
            const auto payload = QJsonDocument::fromJson(body).object();
            Check(method == "POST" && payload["snapshot_id"] == "9000000000000000001" && payload["feature_type"] == "ranking" && payload["version_code"] == 1,
                "Snapshot details use exact JSON fields and preserve the 64-bit string ID");
            return {200, Json({{"status_code", 0}, {"data", QJsonObject{{"user_id_str", detailUser}, {"show_appeal_button", button}, {"status", 0}}}})};
        }
        if (path == "/webcast/eco/check_appeal/") return {200, Json({{"status_code", 0}, {"data", QJsonObject{{"check_status", eligibility}, {"status", true}}}})};
        if (path == "/webcast/perception/appeal/" || path == "/webcast/eco/appeal/") {
            ++mutations; mutationPath = path; mutationBody = QJsonDocument::fromJson(body).object();
            Check(method == "POST" && headers.contains({"Content-Type", "application/json"}), "Appeal submits JSON exactly once through native signing");
            Check(signature["stub"] == QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Md5).toHex()), "Appeal signature covers the exact JSON body");
            if (timeout) return {0, {}, "fixture timeout"};
            if (malformed) return {200, R"({"data":{}})"};
            if (reject) return {200, R"({"status_code":9,"data":{"prompts":"Appeal closed"}})"};
            auto punish = record["punish_info"].toObject(); punish["appeal_status"] = 2; record["punish_info"] = punish;
            return {200, R"({"status_code":0,"data":{}})"};
        }
        throw std::runtime_error("Unexpected safety fixture request");
    };
    auto snapshot = CheckTikTokViolations(context);
    Check(snapshot.error.isEmpty() && snapshot.records.size() == 1 && mutations == 0, "Active records are detected even when the server counter is zero; detection never submits");
    auto fallback = context; fallback.account.live_webcast_host.clear();
    fallback.account.cookie_jar += ".tiktok.com\tTRUE\t/\tTRUE\t0\tstore-country-code\tus\n";
    const int beforeFallback = listCalls; failFirstList = true;
    snapshot = CheckTikTokViolations(fallback);
    Check(snapshot.error.isEmpty() && snapshot.records.size() == 1 && listCalls == beforeFallback + 2 &&
        snapshot.context.account.live_webcast_host == "webcast16-normal-useast5.tiktokv.us", "Successful fallback host clears the previous safety request error");
    auto mobile = context; mobile.account.mobile = true;
    Check(CheckTikTokViolations(mobile).records.size() == 1, "Android uses the same active-list detection contract");
    auto state = LoadTikTokAppeal(context, record);
    Check(state.error.isEmpty() && state.canSubmit && lookupId == "200" && mutations == 0, "Details verify account membership and punishment ID without appealing");
    const auto reasons = TikTokAppealReasons();
    auto result = SubmitTikTokAppeal(state, reasons.back().second, "  Please review this restriction.  ");
    Check(result.submitted && mutations == 1 && !result.canSubmit, "Successful appeal preserves its receipt and refreshes status");
    Check(mutationBody == QJsonObject{{"punish_id", "200"}, {"text_evidence_title", reasons.back().second},
        {"text_evidence_desc", "Please review this restriction."}, {"appeal_req_scene", 4}, {"enter_from", "rg_violation_list"}, {"language", "en"}}, "General appeal matches the Python app's exact payload");
    const auto baseline = mutations;
    Check(!SubmitTikTokAppeal(state, reasons[0].second, {}).submitted && mutations == baseline, "Stale eligibility is rechecked before any mutation");
    record = SafetyRecord(); state = LoadTikTokAppeal(context, record);
    for (const auto& input : QVector<QPair<QString, QString>>{{"", ""}, {"unknown", "text"}, {reasons.back().second, " "},
        {reasons.back().second, QString::fromUtf8("😀").repeated(71)}})
        Check(!SubmitTikTokAppeal(state, input.first, input.second).error.isEmpty() && mutations == baseline, "Reason and UTF-16 length validation block invalid appeal bodies");
    for (auto code : {QJsonValue(0), QJsonValue(2), QJsonValue(3), QJsonValue(9), QJsonValue(true), QJsonValue("1")}) {
        auto punish = record["punish_info"].toObject(); punish["appeal_status"] = code; record["punish_info"] = punish;
        Check(!LoadTikTokAppeal(context, record).canSubmit, "Only numeric unappealed status permits a general appeal");
    }
    record = SafetyRecord();
    for (int copyrightType : {45, 99}) { type = copyrightType; state = LoadTikTokAppeal(context, record); Check(!state.canSubmit && state.reason.contains("copyright") && state.route.officialUrl.host() == "livecenter.tiktok.com", "Copyright goes to the official evidence form"); }
    type = 29; state = LoadTikTokAppeal(context, record);
    user = "999"; Check(!SubmitTikTokAppeal(state, reasons[0].second, {}).error.isEmpty() && mutations == baseline, "Changed account blocks writes"); user = "123";
    listed = false; Check(!SubmitTikTokAppeal(state, reasons[0].second, {}).error.isEmpty() && mutations == baseline, "Missing account membership blocks writes"); listed = true;
    wrongDetail = true; Check(!LoadTikTokAppeal(context, record).error.isEmpty(), "Wrong punishment details are rejected"); wrongDetail = false;
    auto punish = record["punish_info"].toObject(); punish["detail_url"] = "https://livecenter.tiktok.com/punish_detail?punish_id=999"; record["punish_info"] = punish;
    state = LoadTikTokAppeal(context, record); Check(lookupId == "999" && state.route.submissionId == "200", "Lookup alias is distinct from verified submission ID");
    Check(SubmitTikTokAppeal(state, reasons[0].second, "ignored").submitted && mutationBody["punish_id"] == "200" && mutationBody["text_evidence_desc"] == "", "Normal reasons omit hidden explanation and submit the verified ID");
    for (int failure = 0; failure < 3; ++failure) {
        record = SafetyRecord(); state = LoadTikTokAppeal(context, record); const auto before = mutations;
        timeout = failure == 0; malformed = failure == 1; reject = failure == 2;
        result = SubmitTikTokAppeal(state, reasons[0].second, {});
        Check(!result.submitted && !result.error.isEmpty() && mutations == before + 1, "Timeouts, malformed responses and rejections never retry or claim success");
    }
    timeout = malformed = reject = false; record = SafetyRecord(); state = LoadTikTokAppeal(context, record);
    mutations = 0; refreshFailure = true; result = SubmitTikTokAppeal(state, reasons[0].second, {});
    Check(result.submitted && result.error.isEmpty() && result.reason.contains("accepted") && mutations == 1, "Refresh failure after success retains the appeal receipt"); refreshFailure = false;
    record = SafetyRecord(); punish = record["punish_info"].toObject();
    punish["detail_url"] = "https://livecenter.tiktok.com/treco/ban_detail?snapshot_id=9000000000000000001&feature_type=ranking"; record["punish_info"] = punish;
    state = LoadTikTokAppeal(context, record);
    Check(state.canSubmit && state.status == "Not appealed", "Snapshot restrictions have their own status enum");
    const auto previous = state;
    for (auto code : {QJsonValue(2), QJsonValue(3), QJsonValue(4), QJsonValue(99), QJsonValue(true)}) { eligibility = code; Check(!LoadTikTokAppeal(context, record).canSubmit, "Snapshot check status overrides legacy true eligibility"); }
    eligibility = 1; detailUser = "999"; Check(!LoadTikTokAppeal(context, record).error.isEmpty(), "Snapshot ownership is verified"); detailUser = "123";
    button = false; Check(!LoadTikTokAppeal(context, record).canSubmit, "Snapshot appeal requires the server's show-button grant"); button = true;
    result = SubmitTikTokAppeal(previous, {}, {});
    Check(result.submitted && mutationPath == "/webcast/eco/appeal/" && mutationBody == QJsonObject{{"snapshot_id", "9000000000000000001"}}, "Snapshot submits the original app's exact one-field payload");
    record = SafetyRecord(); state = LoadTikTokAppeal(context, record);
    punish = record["punish_info"].toObject(); punish["detail_url"] = previous.route.officialUrl.toString(); record["punish_info"] = punish;
    const auto beforeRoute = mutations; Check(!SubmitTikTokAppeal(state, reasons[0].second, {}).error.isEmpty() && mutations == beforeRoute, "Changed appeal route requires review before submission");
    for (const QString& link : {QString("javascript:alert(1)"), QString("https://tiktok.com.evil.test/punish_detail?record_id=9"),
        QString("https://user:pass@livecenter.tiktok.com/foo"), QString("https://livecenter.tiktok.com:8000/foo"),
        QString("https://livecenter.tiktok.com/foo?record_id=1&record_id=2")}) {
        auto untrusted = SafetyRecord(); auto p = untrusted["punish_info"].toObject(); p.remove("punish_record_id_str"); p["detail_url"] = link; untrusted["punish_info"] = p;
        Check(TikTokViolationRoute(untrusted).officialUrl.isEmpty(), "External navigation rejects untrusted or ambiguous URLs");
    }
    auto wrapped = SafetyRecord(); punish = wrapped["punish_info"].toObject();
    punish["detail_url"] = "sslocal://webcast_webview?url=" + QString::fromLatin1(QUrl::toPercentEncoding(punish["detail_url"].toString())); wrapped["punish_info"] = punish;
    Check(TikTokViolationRoute(wrapped).id == "200", "Official in-app URL wrappers resolve without navigating arbitrary schemes");
    // The automatic popup inherits the ID from room creation; account info
    // can return that same 64-bit ID as a string on the next request.
    const auto numericRoom = QJsonDocument::fromJson(R"({"owner_user_id":9000000000000000001})").object();
    const auto exactUser = QStringLiteral("9000000000000000001");
    Check(tiktok_studio_account_user_id(QJsonDocument::fromJson(R"({"user":{"user_id":9000000000000000001}})").object()) == exactUser,
        "Numeric account IDs retain every digit through nested account lookup");
    for (const auto invalid : {QJsonValue(1.5), QJsonValue(-1), QJsonValue(true), QJsonValue(0), QJsonValue(1e25)})
        Check(tiktok_studio_room_owner_id({{"owner_user_id", invalid}}).isEmpty(), "Invalid numeric account IDs remain rejected");
    for (bool useMobile : {false, true}) for (bool useNumericAccount : {false, true}) {
        numericAccount = useNumericAccount;
        auto streaming = context;
        streaming.userId = tiktok_studio_room_owner_id(numericRoom);
        streaming.account.user_id = streaming.userId;
        streaming.account.mobile = useMobile;
        streaming.pcProfile = {{"device_id", "11"}, {"install_id", "12"}};
        user = exactUser; record = SafetyRecord();
        const auto detected = CheckTikTokViolations(streaming);
        auto details = LoadTikTokAppeal(detected.context, detected.records.front());
        Check(details.error.isEmpty() && details.canSubmit && details.context.userId == exactUser,
            "Automatic violation details accept the same large account ID from numeric or string responses");
        details = LoadTikTokAppeal(details.context, details.record);
        Check(details.error.isEmpty() && details.canSubmit, "Refresh details keeps the verified account identity");
        const auto before = mutations;
        const auto appealed = SubmitTikTokAppeal(details, reasons[0].second, {});
        Check(appealed.error.isEmpty() && appealed.submitted && mutations == before + 1,
            "Desktop/mobile automatic popup submits once for the same exact account ID");
        user = "9000000000000000002";
        Check(!SubmitTikTokAppeal(details, reasons[0].second, {}).error.isEmpty() && mutations == before + 1,
            "An actually different account still blocks appeals, including IDs differing only in their last digit");
    }
    std::cout << "PASS: active violation detection, account-bound general/snapshot appeal contracts, exact JSON, eligibility and no mutation retries\n";
}
