// Offline protocol fixtures; never contacts RapidAPI or TikTok.
#include "live_client.hpp"
#include "mobile_profile.hpp"
#include "mobile_device.hpp"
#include "frame_signing.hpp"
#include "rapidapi_quota.hpp"
#include "sei_metadata.hpp"
#include "tiktok_request_signer.hpp"
#include "tiktok_studio_device.hpp"
#include "tiktok_studio_session.hpp"
#include "http_fixture.hpp"
#include "account-info.h"
#include "streamlabs-info.h"
#include "tiktok-game-info.h"
#include "request_cancellation.hpp"
#include "domain_routing.hpp"
#include "api_error.hpp"
#include "qr_login.hpp"
#include "tiktok_studio_qr.hpp"
#include "streamlabs-desktop.h"
#include <QImage>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <iostream>
#include <stdexcept>

QString text(const char *key) { return QString::fromUtf8(key); }
QString translated_or(const char *, const QString &fallback) { return fallback; }
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QByteArray Json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
#include "live-safety-fixtures.hpp"

void CookieAndDeviceFixtures()
{
    QTemporaryDir temp;
    const auto path = temp.filePath("cookies 日本語.json");
    QFile file(path);
    Check(file.open(QIODevice::WriteOnly), "create cookie fixture");
    file.write(R"([{"domain":".tiktok.com","name":"sessionid","value":"dummy-session"},
        {"domain":"unrelated.example","name":"private","value":"never-send"}])");
    file.close();
    QString error;
    const auto jar = read_tiktok_cookie_file(path, &error);
    Check(error.isEmpty() && jar.contains("dummy-session") && !jar.contains("never-send"), "import only TikTok cookies");
    for (const auto payload : {
        QByteArray(R"([{"domain":"","name":"sessionid","value":"python-session","path":"/","secure":false},
            {"name":"sessionid_ss","value":"python-session"},{"domain":"","name":"store-idc","value":"alisg"},
            {"domain":"unrelated.example","name":"private","value":"never-send"}])"),
        QByteArray(R"({"cookies":[{"name":"sid_tt","value":"python-session"},
            {"name":"store-idc","value":"alisg"}]})")}) {
        Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite Python cookie fixture");
        file.write(payload); file.close();
        const auto imported = read_tiktok_cookie_file(path, &error);
        Check(error.isEmpty() && imported.contains(".tiktok.com\tTRUE\t/\tTRUE\t0\t") &&
            !TikTokSafetyCookieIdentity(imported).isEmpty() && tiktok_cookie_value(imported, "store-idc") == "alisg" &&
            !imported.contains("never-send"), "Python cookies with blank or missing domains retain their login and routing cookies, scoped to TikTok");
    }
    for (const auto payload : {
        QByteArray(R"([{"domain":"unrelated.example","name":"sessionid","value":"not-tiktok"}])"),
        QByteArray(R"([{"domain":false,"name":"sessionid","value":"invalid-domain"}])"),
        QByteArray(R"([{"name":"sessionid","value":""},{"name":"other","value":"sessionid=not-a-login"}])")}) {
        Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite invalid session fixture");
        file.write(payload); file.close();
        Check(read_tiktok_cookie_file(path, &error).isEmpty() && !error.isEmpty(),
            "Unrelated domains, malformed domains and empty or substring-only sessions are rejected");
    }
    Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite fixture");
    file.write("# Netscape HTTP Cookie File\n#HttpOnly_.tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tdummy\n");
    file.close();
    Check(!read_tiktok_cookie_file(path, &error).isEmpty() && error.isEmpty(), "Netscape HttpOnly cookie import");
    Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite invalid fixture");
    file.write("[]"); file.close();
    Check(read_tiktok_cookie_file(path, &error).isEmpty() && !error.isEmpty(), "empty cookies rejected before requests");
    const auto encrypted = encrypt_tiktok_device_registration_payload("{\"hello\":\"world\"}", 1700000000);
    Check(encrypted == QByteArray::fromHex("7463030b0003fbcc2b94447748e36092f9046c26b920e6aa2cf76f8f7165ae48a"
        "1aeb031d51e5a737894ceb12ee32908b1549a79c0b6"), "desktop device encryption matches upstream fixture");
}

void MobileDeviceFixtures()
{
    // The hash is the original app's independently captured native vector.
    const auto encrypted = encrypt_mobile_registration(QByteArray(16, 'a'), QByteArray(32, char(12345678 & 255)));
    Check(QCryptographicHash::hash(encrypted, QCryptographicHash::Sha256).toHex() ==
        "4cd42c6b1db433a09d3fbe9736acf8525c5859cf3c8210290990079fd18b0a5e", "Android ttEncrypt matches original native vector");
    const auto profile = complete_mobile_device_profile({{"broadcast_type", "screen_record"},
        {"fingerprint", QJsonObject{{"model", "Pixel 7"}}}, {"identifiers", QJsonObject{{"android_id", "abcdef0123456789"}}}});
    Check(profile.value("fingerprint").toObject().value("os_api") == "33" &&
        profile.value("identifiers").toObject().value("openudid") == "abcdef0123456789", "Partial profiles retain matching hardware and Android identity");
    Check(complete_mobile_device_profile(profile) == profile, "Completing an existing profile never rotates its identity");
    auto inspect = profile;
    inspect["registration_options"] = QJsonObject{{"encoding", "json"}, {"host", "api-boot.tiktokv.com"}};
    const auto plain = prepare_mobile_registration(inspect, 1700000000000, "00000000-0000-0000-0000-000000000001");
    const auto header = QJsonDocument::fromJson(plain.body).object().value("header").toObject();
    Check(header.value("aid").toInt() == 1233 && header.value("version_code").toInt() == 470104 &&
        header.value("manifest_version_code").toInteger() == 2024701040 && header.value("sdk_version") == "2.5.14.6", "Android registration app/build/SDK versions match reference");
    Check(header.value("openudid") == "abcdef0123456789" && !header.contains("device_id") &&
        QUrlQuery(plain.url).queryItemValue("device_platform") == "android", "Bootstrap uses generated fingerprint without invented server IDs");
    Check(plain.headers.contains({"x-ss-stub", QCryptographicHash::hash(plain.body, QCryptographicHash::Md5).toHex()}), "Registration hashes transmitted bytes");
    const auto wire = prepare_mobile_registration(profile);
    Check(wire.body.startsWith(QByteArray::fromHex("746305100000")) && wire.headers.contains({"x-tt-request-tag", "t=0;n=1"}) &&
        wire.url.host() == "log-va.tiktokv.com", "Default transport is local ttEncrypt and direct TikTok registration");
    Check(prepare_mobile_registration(profile).body != wire.body, "Android encryption uses fresh random seeds");
    for (const auto bad : {QJsonObject{{"device_id", 0}, {"install_id", 42}}, QJsonObject{{"device_id", true}, {"install_id", 42}},
        QJsonObject{{"device_id", "123"}}, QJsonObject{{"device_id", 1.5}, {"install_id", 42}},
        QJsonObject{{"device_id", "oops"}, {"install_id", 42}}}) {
        bool rejected = false;
        try { parse_mobile_registration(bad); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "Zero, partial, non-integer and malformed registration IDs rejected");
    }
    TikTokStudioAccountCredentials account;
    account.mobile = true;
    account.cookie_jar = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tfixture\n";
    account.device_profile = profile;
    int requests = 0;
    bool fail = true;
    tiktok_http_fixture = [&](const QByteArray& method, const QUrl& url, const TikTokHttpHeaders& headers, const QByteArray& body) {
        ++requests;
        Check(method == "POST" && url.host() == "log-va.tiktokv.com" && url.path() == "/service/2/device_register/", "Android registration uses only the direct bootstrap endpoint");
        Check(headers.contains({"x-ss-stub", QCryptographicHash::hash(body, QCryptographicHash::Md5).toHex()}), "Native request sends the hashed registration bytes");
        for (const auto& [name, value] : headers) Check(name.toLower() != "x-rapidapi-key" && name.toLower() != "cookie", "Bootstrap sends no account credentials");
        return TikTokHttpFixtureResponse{200, fail ? QByteArray(R"({"status_code":7,"message":"Error","data":{"description":"Android device registration denied"}})") :
            QByteArray(R"({"data":{"device_id_str":"9876543210987654321","install_id_str":"8765432109876543210","device_token":"fixture","ssid":"saved"}})")};
    };
    Check(initialize_tiktok_account(account).contains("Android device registration denied") && !account.has_device(), "Registration preserves the specific rejection without a usable identity");
    const auto local = account.device_profile;
    fail = false;
    Check(initialize_tiktok_account(account).isEmpty() && account.has_device() && requests == 2, "Blank Android IDs register without a RapidAPI key");
    Check(account.device_id == "9876543210987654321" && account.device_profile.value("registration").toObject().value("ssid") == "saved", "Registration retains exact IDs and metadata");
    Check(account.device_profile.value("identifiers") == local.value("identifiers") && account.device_profile.value("fingerprint") == local.value("fingerprint"), "Retry preserves generated local identity");
    const auto saved = account.device_profile;
    Check(initialize_tiktok_account(account).isEmpty() && account.device_profile == saved && requests == 2, "Saved Android IDs are reused without registration");
    account.install_id.clear();
    Check(!initialize_tiktok_account(account).isEmpty() && requests == 2, "Partial manual Android IDs do not silently rotate the installation");
}

void LiveFixtures(bool mobile, bool lostResponse, bool incompleteRoom,
    const QString &topic = "5", const QString &broadcast = "video", const QString &orientation = "1",
    const TikTokLiveSettings& settings = {})
{
    TikTokStudioAccountCredentials account;
    account.mobile = mobile;
    account.device_profile = {{"broadcast_type", broadcast}, {"orientation", orientation}};
    account.rapidapi_key = "fixture-key";
    account.device_id = "7000000000000000001";
    account.install_id = "7000000000000000002";
    account.cookie_jar = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tdummy\n"
        ".tiktok.com\tTRUE\t/\tTRUE\t0\tstore-idc\tc-alisg\n";
    int creates = 0, finishes = 0, prepares = 0;
    bool created = false;
    QByteArray signedBody, signedQuery;
    QJsonObject room{{"id_str", "9001"}, {"stream_id_str", "9002"}, {"owner_user_id_str", "9003"},
        {"stream_url", QJsonObject{{"rtmp_push_url", incompleteRoom ? "bad-url" : "rtmp://ingest.example/live/key?fixture=1"}}}};
    tiktok_http_fixture = [&](const QByteArray &method, const QUrl &url, const TikTokHttpHeaders &headers,
                              const QByteArray &body) -> TikTokHttpFixtureResponse {
        const auto path = url.path();
        if (path == "/get_domains/v4/") return {200, "{}"};
        if (path == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"gb"}})"};
        if (path == "/signatures") {
            const auto request = QJsonDocument::fromJson(body).object();
            Check(request.value("aid").toString() == (mobile ? "1233" : "8311"), "signer aid matches selected client");
            signedQuery = request.value("params").toString().toUtf8();
            if (mobile) {
                Check(request.value("implementation") == "mobile" && request.value("install_id") == account.install_id,
                    "Android signer gets implementation and install ID");
                signedBody = QByteArray::fromBase64(request.value("body_base64").toString().toLatin1());
                Check(!request.value("url").toString().isEmpty() && request.value("headers").isObject(), "Android signer receives URL and headers");
            }
            return {200, R"({"headers":{"x-khronos":"1700000000","x-ladon":"ladon","x-argus":"argus","x-gorgon":"gorgon"}})"};
        }
        Check(url.host().endsWith("tiktokv.com"), "requests only target expected TikTok region");
        if (path == "/webcast/room/create/") {
            ++creates;
            Check(method == "POST", "create uses POST");
            QUrlQuery query(url), form(QString::fromUtf8(body));
            Check(form.allQueryItemValues("gift_auth") == QStringList{settings.allowGifts ? "1" : "2"} &&
                form.allQueryItemValues("chat_auth") == QStringList{settings.allowComments ? "1" : "2"}, "Gifts/comments use one explicit AuthSwitch field: 1=on, 2=off");
            const bool own = settings.contentDisclosure == "own_brand" || settings.contentDisclosure == "both";
            const bool third = settings.contentDisclosure == "third_party" || settings.contentDisclosure == "both";
            Check(form.queryItemValue("open_commercial_content_toggle") == (own || third ? "true" : "false") &&
                form.queryItemValue("commercial_content_promote_myself") == (own ? "true" : "false") &&
                form.queryItemValue("commercial_content_promote_third_party") == (third ? "true" : "false"), "Disclosure booleans match the original app's four choices");
            Check(third ? form.queryItemValue("ecom_bc_toggle") == "5" : mobile ? form.queryItemValue("ecom_bc_toggle") == "0" :
                !form.hasQueryItem("ecom_bc_toggle"), "Ecommerce field follows client-specific off/own/third-party rules");
            Check(form.queryItemValue("title", QUrl::FullyDecoded) == "LIVE + music & café", "title form encoding preserves plus, ampersand and Unicode");
            Check(query.queryItemValue("aid") == (mobile ? "1233" : "8311"), "client query aid");
            Check(query.queryItemValue(mobile ? "iid" : "install_id") == account.install_id, "correct install ID query name");
            Check(form.queryItemValue("hashtag_id") == topic, "Selected topic reaches the room create request");
            if (!mobile || broadcast == "screen_record")
                Check(form.queryItemValue("game_tag_id") == (topic == "5" ? "202" : "0"), "Only Gaming sends its selected game ID");
            if (mobile && broadcast == "screen_record") {
                Check(form.queryItemValue("screen_shot") == "1" && form.queryItemValue("live_room_mode") == "4" &&
                    form.queryItemValue("orientation") == orientation, "Mobile gaming reproduces screen capture mode and selected orientation");
            } else if (mobile) {
                for (const auto field : {"game_tag_id", "orientation", "screen_shot", "live_room_mode"})
                    Check(!form.hasQueryItem(field), "Camera LIVE must omit every screen capture field");
            }
            if (mobile) {
                Check(!form.hasQueryItem("live_studio") && !query.hasQueryItem("live_mode"), "Android camera LIVE excludes desktop fields");
                Check(form.queryItemValue("chat_l_2") == "1" && form.queryItemValue("age_restricted") == "4", "mobile LIVE options");
                Check(body == signedBody && url.query(QUrl::FullyEncoded).toUtf8() == signedQuery, "sign exactly the transmitted mobile bytes");
                bool gorgon = false;
                for (const auto &[name, value] : headers) if (name == "X-Gorgon" && value == "gorgon") gorgon = true;
                Check(gorgon, "Android signed request includes X-Gorgon");
            } else Check(form.queryItemValue("live_studio") == "1", "desktop mode retained");
            created = true;
            if (lostResponse) return {0, {}, "Response lost after room creation"};
            return {200, Json({{"status_code", 0}, {"data", room}})};
        }
        if (path == "/webcast/room/continue/")
            return {200, Json({{"status_code", 0}, {"data", created ? QJsonObject{{"room", room}} : QJsonObject{}}})};
        if (path == "/webcast/room/ping/anchor/") {
            QUrlQuery form(QString::fromUtf8(body));
            if (form.queryItemValue("status") == "4") ++finishes;
            if (form.queryItemValue("status") == "1") ++prepares;
            return {200, "{\"status_code\":0}"};
        }
        if (path == "/webcast/room/anchor_pre_finish/") return {200, "{\"status_code\":0}"};
        throw std::runtime_error("Unexpected TikTok endpoint");
    };
    Check(initialize_tiktok_account(account).isEmpty(), "existing device identity used");
    QString error;
    auto live = create_tiktok_live(account, "LIVE + music & café", topic, "202", true, &error, false, settings);
    Check(creates == 1, "uncertain create recovers instead of duplicating room");
    Check(live.room_id == "9001" && live.stream_id == "9002", "room IDs retained even for cleanup after bad response");
    if (incompleteRoom) Check(!error.isEmpty(), "bad RTMP URL fails startup");
    else Check(error.isEmpty() && live.key == "key?fixture=1" && prepares == 1, "create/prepare returns usable RTMP credentials");
    Check(end_tiktok_live(live).isEmpty() && finishes == 2 && live.room_id.isEmpty(), "end sends finish pings and clears owned room");
    Check(end_tiktok_live(live).isEmpty() && finishes == 2, "end is idempotent");
}

void GameCatalogueFixtures()
{
    const auto payload = QJsonDocument::fromJson(R"({"data":{"game_tag_list":[
        {"id":101,"show_name":"League of Legends"},{"id":"202","show_name":"Fortnite"},
        {"id":"202","show_name":"Duplicate"},{"id":"303"},{"show_name":"No ID"},null]}})").object();
    const auto tags = parse_tiktok_studio_game_tags(payload);
    Check(tags.size() == 2 && tags[0].id == "101" && tags[1].name == "Fortnite", "Game parser preserves IDs/names and ignores malformed and duplicate rows");
    int calls = 0;
    bool fail = false;
    tiktok_http_fixture = [&](const QByteArray &method, const QUrl &url, const TikTokHttpHeaders &, const QByteArray &body) {
        ++calls;
        Check(method == "GET" && url.path() == "/webcast/room/hashtag/list/" && body.isEmpty() && url.query().isEmpty(),
            "Game catalogue uses unsigned GET without category search text or a RapidAPI request");
        if (!fail) Check(url.host() == (calls == 1 ? "webcast16-normal-c-alisg.tiktokv.com" : "webcast16-normal-no1a.tiktokv.eu"),
            "Game catalogue uses the account IDC before the regional fallback");
        if (fail || calls == 1) return TikTokHttpFixtureResponse{503, "{}"};
        return TikTokHttpFixtureResponse{200, Json(payload)};
    };
    QString error;
    const QByteArray cookies = ".tiktok.com\tTRUE\t/\tTRUE\t0\tstore-idc\tc-alisg\n.tiktok.com\tTRUE\t/\tTRUE\t0\tstore-country-code\tfr\n";
    Check(fetch_tiktok_game_tags(cookies, &error).size() == 2 && error.isEmpty() && calls == 2,
        "Game catalogue falls back after an account-host transport failure");
    fail = true;
    Check(fetch_tiktok_game_tags(cookies, &error).isEmpty() && !error.isEmpty(), "Catalogue errors remain visible");
    const int before = calls;
    std::atomic_bool cancelled{true};
    TikTokRequestCancellation scope(&cancelled);
    Check(fetch_tiktok_game_tags(cookies, &error).isEmpty() && calls == before, "Cancelled game fetch performs no HTTP requests");
}

void QuotaFixtures()
{
    const qint64 observed = 1800000000;
    const auto relative = ParseRapidApiQuota({{"X-RateLimit-Requests-Limit", "10000"},
        {"x-ratelimit-requests-remaining", "9437"}, {"x-ratelimit-requests-reset", "3600"}}, observed);
    Check(relative.remaining == 9437 && relative.limit == 10000 && relative.reset_epoch_seconds == observed + 3600,
        "Quota headers ignore case and resolve reset countdown from observation time");
    const auto absolute = ParseRapidApiQuota({{"x-quota-limit", "100"}, {"x-quota-used", "90"},
        {"x-quota-reset", "1900000000"}}, observed);
    Check(absolute.remaining == 10 && absolute.reset_epoch_seconds == 1900000000, "Quota aliases, used fallback and absolute reset timestamp");
    Check(!ParseRapidApiQuota({{"x-quota-limit", "99999999999999999999999"}, {"x-quota-remaining", "-4"}}, observed).known(),
        "Malformed and overflowing quota numbers stay unknown");
    QTemporaryDir temp;
    SetRapidApiQuotaDirectory(temp.path());
    HostedSigningServiceConfig service; service.api_key = "quota-fixture-secret";
    int calls = 0;
    TikTokHttpFixtureResponse response{200, R"({"x-khronos":"1","x-ladon":"fixture","x-argus":"fixture","x-gorgon":"fixture"})"};
    response.headers = {{"x-quota-limit", "10000"}, {"x-quota-remaining", "9437"}, {"x-quota-reset", "3600"}};
    tiktok_http_fixture = [&](const QByteArray&, const QUrl&, const TikTokHttpHeaders&, const QByteArray&) { ++calls; return response; };
    TikTokRequestSignatureInput request; request.timestamp_seconds = observed;
    for (bool mobile : {false, true}) {
        request.mobile = mobile;
        Check(RapidApiRequestSigner::fetch(service, request).valid(), "Quota observation retains valid desktop/mobile signatures");
    }
    Check(ReadRapidApiQuota(service).remaining == 9437 && calls == 2, "Request signer updates shared quota without extra HTTP calls");
    response.status = 429; response.body = R"({"message":"quota exhausted"})";
    response.headers["x-quota-remaining"] = "0";
    Check(!RapidApiRequestSigner::fetch(service, request).valid() && ReadRapidApiQuota(service).remaining == 0,
        "Rejected request still updates exhausted quota");
    FrameSignInput frame;
    response.status = 200;
    response.body = R"([{"timestamp":1800000000,"signinfo":"fixture","signvalue":"fixture","signversion":"1.0"}])";
    response.headers["x-quota-remaining"] = "8100";
    Check(FrameSignClient::fetch_batch(service, frame, observed).valid() && ReadRapidApiQuota(service).remaining == 8100,
        "Desktop frame signing refreshes the same subscription meter");
    frame.mobile = true;
    response.body = R"({"signinfo":"fixture","signvalue":"fixture","signversion":"2.0"})";
    response.headers["x-quota-remaining"] = "8000";
    Check(FrameSignClient::fetch_batch(service, frame, observed).valid() && ReadRapidApiQuota(service).remaining == 8000,
        "Android frame signing refreshes the same subscription meter");
    response.status = 429; response.body = R"({"message":"quota exhausted"})"; response.headers["x-quota-remaining"] = "0";
    Check(!FrameSignClient::fetch_batch(service, frame, observed).valid() && ReadRapidApiQuota(service).remaining == 0,
        "Frame signer rejection also reports exhausted quota");
    response.headers.clear(); RapidApiRequestSigner::fetch(service, request);
    Check(ReadRapidApiQuota(service).remaining == 0, "Responses without quota headers preserve the latest observation");
    auto other = service; other.api_key = "another-key";
    Check(!ReadRapidApiQuota(other).known(), "Different API keys cannot show each other's usage");
    other = service; other.base_url = QUrl("https://another-signer.p.rapidapi.com/");
    Check(!ReadRapidApiQuota(other).known(), "Different RapidAPI services have separate meters");
    const int before = calls;
    SetRapidApiQuotaDirectory(temp.path());
    Check(ReadRapidApiQuota(service).remaining == 0 && calls == before, "Persisted quota reloads without any API calls");
    const auto files = QDir(temp.path()).entryList({"*.json"}, QDir::Files);
    Check(files.size() == 1 && !files.front().contains(service.api_key), "Cache filenames do not contain API keys");
    QFile file(temp.filePath(files.front())); Check(file.open(QIODevice::ReadOnly), "Read quota snapshot");
    Check(!file.readAll().contains(service.api_key.toUtf8()), "Quota cache stores no API keys");
    SetRapidApiQuotaDirectory({});
}

void SigningFixtures()
{
    HostedSigningServiceConfig service;
    service.api_key = "dummy";
    FrameSignInput input{"1233", "1", "2", "3", "2", 0, true, mobile_frame_context({})};
    bool versionTwo = true;
    tiktok_http_fixture = [&](const QByteArray &, const QUrl &url, const TikTokHttpHeaders &, const QByteArray &body) {
        Check(url.path() == "/framesign", "Android uses single frame signer endpoint");
        const auto request = QJsonDocument::fromJson(body).object();
        Check(request.value("implementation") == "mobile" && request.value("sdk_context").toObject().value("metadata_field6") == 270088,
            "Android frame SDK context is transmitted");
        return TikTokHttpFixtureResponse{200, Json({{"signResult", QJsonObject{{"frametype", "2"}, {"lid", "1877999593"},
            {"signinfo", "info"}, {"signvalue", "value"}, {"signversion", versionTwo ? "2.0" : "1.0"}}}})};
    };
    auto batch = FrameSignClient::fetch_batch(service, input, 1700000000);
    Check(batch.valid() && batch.first_timestamp() == 1700000000, "Android signature parsed at requested timestamp");
    SignedSeiSession sei({"1233", "1", "2", "3", true});
    sei.merge_signatures(batch.signatures);
    QByteArray packet, replacement;
    packet = QByteArray::fromHex("00000001658899");
    QString error;
    Check(sei.transform_packet(SignedVideoCodec::H264, packet, 0, 1080, 1920, 1700000000000, &replacement, &error), "Android packet receives SEI");
    Check(replacement.contains("2.0") && !replacement.contains("ttls_live_scene"), "Android SEI uses version 2 without Studio marker");
    versionTwo = false;
    Check(!FrameSignClient::fetch_batch(service, input, 1700000000).valid(), "desktop signature rejected for Android");
    Check(!RapidApiRequestSigner::parse_response("{\"message\":\"rate limit\"}", 429).valid(), "rate limit stops request signing");
    Check(!RapidApiRequestSigner::parse_response("{\"x-khronos\":\"1\"}", 200).valid(), "incomplete signatures rejected");
}

void AccountFixtures()
{
    std::atomic_bool cancelled{false};
    OutputTargetConfig config;
    config.connectionMethod = ConnectionMethod::Streamlabs;
    config.streamlabsAccessToken = "dummy-token";
    long status = 200;
    QByteArray response = R"({"user":{"username":"streamlabs_account"},"application_status":{"status":"approved"},"can_be_live":true})";
    int calls = 0;
    tiktok_http_fixture = [&](const QByteArray &method, const QUrl &url, const TikTokHttpHeaders &headers,
                              const QByteArray &body) {
        ++calls;
        Check(method == "GET" && url == QUrl("https://streamlabs.com/api/v5/slobs/tiktok/info") && body.isEmpty(),
            "Streamlabs account lookup only reads the documented info endpoint");
        Check(headers.contains({"Authorization", "Bearer dummy-token"}), "Token sent in Bearer header");
        return TikTokHttpFixtureResponse{status, response};
    };
    Check(LookupAccountInfo(config, cancelled).username == "streamlabs_account", "Streamlabs reads user.username");
    auto account = LookupAccountInfo(config, cancelled);
    Check(account.applicationStatus == "approved" && account.canGoLive == std::optional<bool>(true), "Status and live access read from the info response");
    response = R"({"user":{"username":"streamlabs_account"},"application_status":{"status":"pending"},"can_be_live":false})";
    account = LookupAccountInfo(config, cancelled);
    Check(account.applicationStatus == "pending" && account.canGoLive == std::optional<bool>(false), "Explicit false live access is preserved");
    response = R"({"user":{"username":"streamlabs_account"}})";
    Check(!LookupAccountInfo(config, cancelled).canGoLive.has_value(), "Missing live-access field remains unknown");
    response = R"({"application_status":{"status":"pending"},"can_be_live":false})";
    account = LookupAccountInfo(config, cancelled);
    Check(!account.errorKey && account.username.isEmpty() && account.applicationStatus == "pending" && account.canGoLive == std::optional<bool>(false),
        "Status and live access can be read independently of username availability");
    for (long code : {401, 403, 429, 500, 302}) {
        status = code;
        const auto result = LookupAccountInfo(config, cancelled);
        Check(result.username.isEmpty() && result.errorKey, "HTTP errors cannot display a stale username");
    }
    status = 200;
    for (const QByteArray &invalid : {QByteArray("not-json"), QByteArray("{}"),
            QByteArray(R"({"success":false,"user":{"username":"wrong"}})")}) {
        response = invalid;
        Check(LookupAccountInfo(config, cancelled).errorKey, "Malformed or unsuccessful account responses rejected");
    }
    const auto before = calls;
    cancelled = true;
    LookupAccountInfo(config, cancelled);
    Check(calls == before, "Cancelled lookup never contacts the service");
    cancelled = false;

    QTemporaryDir temp;
    QFile cookies(temp.filePath("cookies.json"));
    Check(cookies.open(QIODevice::WriteOnly), "create account cookie fixture");
    cookies.write(R"([{"domain":".tiktok.com","name":"sessionid","value":"dummy"},
        {"domain":".tiktok.com","name":"store-idc","value":"c-alisg"}])");
    cookies.close();
    config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.cookieFilePath = cookies.fileName().toStdString();
    config.rapidApiKey = "dummy-key";
    for (bool mobile : {false, true}) {
        config.tiktokClient = mobile ? "mobile" : "pc";
        config.tiktokDeviceProfiles = {{"mobile", {{"device_id", "11"}, {"install_id", "12"}}}};
        bool reject = false;
        int accountReads = 0, registrations = 0;
        tiktok_http_fixture = [&](const QByteArray &method, const QUrl &url, const TikTokHttpHeaders &, const QByteArray &body) {
            if (url.path() == "/get_domains/v4/") return TikTokHttpFixtureResponse{200, "{}"};
            if (url.path() == "/webcast/game/basic/region_check/") return TikTokHttpFixtureResponse{200, R"({"status_code":0,"data":{"priority_region":"gb"}})"};
            if (url.path() == "/service/2/device_register/") {
                ++registrations;
                return TikTokHttpFixtureResponse{200, R"({"device_id_str":"11","install_id_str":"12"})"};
            }
            if (url.path() == "/signatures") {
                Check(method == "POST", "Only the signer accepts POST during account lookup");
                const auto request = QJsonDocument::fromJson(body).object();
                Check(request.value("aid").toString() == (mobile ? "1233" : "8311"), "Account signer uses selected client");
                return TikTokHttpFixtureResponse{200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"};
            }
            if (url.path() == "/webcast/game/basic/create_info/")
                return TikTokHttpFixtureResponse{200, R"({"status_code":0,"data":{"allow_multi_stream":true}})"};
            if (url.path() == "/webcast/anchor_tool/threshold/check/")
                return TikTokHttpFixtureResponse{200, R"({"status_code":0,"data":{"allowed":true,"dual_canvas_used":false}})"};
            ++accountReads;
            Check(method == "GET" && url.path() == "/passport/account/info/v2/" && body.isEmpty(),
                "Username lookup never registers a device, starts or ends a room");
            const QUrlQuery query(url);
            Check(query.queryItemValue("aid") == (mobile ? "1233" : "8311"), "Selected client reaches account endpoint");
            if (mobile) Check(query.queryItemValue("iid") == "12", "Mobile account lookup uses saved Android identity");
            else Check(query.queryItemValue("device_id") == "0" && query.hasQueryItem("sign"),
                "Desktop account lookup works without registering a device");
            return TikTokHttpFixtureResponse{200, reject ? QByteArray(R"({"data":{"error_code":7,"username":"wrong"}})")
                : QByteArray(R"({"data":{"screen_name":"cookie_account"}})")};
        };
        const auto foundAccount = LookupAccountInfo(config, cancelled);
        Check(foundAccount.username == "cookie_account" && foundAccount.canDualLayout == true && accountReads == 1,
            "Cookie account username supports screen_name fallback for both clients");
        reject = true;
        const auto error = LookupAccountInfo(config, cancelled);
        Check(error.username.isEmpty() && error.errorKey, "Rejected cookie session cannot look like a successful identity check");
        if (mobile) {
            reject = false;
            config.tiktokDeviceProfiles = nlohmann::json::object();
            const auto automatic = LookupAccountInfo(config, cancelled);
            Check(automatic.username == "cookie_account" && automatic.canDualLayout == true && registrations == 1 &&
                automatic.deviceProfile["device_id"] == "11" && automatic.deviceProfile.contains("fingerprint"),
                "Account lookup automatically registers missing Android IDs before checking dual access");
            config.tiktokDeviceProfiles["mobile"] = automatic.deviceProfile;
            Check(LookupAccountInfo(config, cancelled).username == "cookie_account" && registrations == 1,
                "Saved account profile avoids repeated Android registrations");
        }
    }
}

void CategoryFixtures()
{
    std::atomic_bool cancelled{false};
    QString sentQuery;
    long status = 200;
    int calls = 0;
    const QString fullName = "A full category name longer than twenty-five characters";
    tiktok_http_fixture = [&](const QByteArray &method, const QUrl &url, const TikTokHttpHeaders &headers, const QByteArray &body) {
        ++calls;
        Check(method == "GET" && url.host() == "streamlabs.com" && url.path() == "/api/v5/slobs/tiktok/info" && body.isEmpty(), "Category search is a read-only info request");
        Check(headers.contains({"Authorization", "Bearer dummy"}), "Category search uses the selected token");
        sentQuery = QUrlQuery(url).queryItemValue("category", QUrl::FullyDecoded);
        Check(sentQuery.toUcs4().size() <= 25, "HTTP category search is limited to 25 Unicode characters");
        return TikTokHttpFixtureResponse{status, Json({{"categories", QJsonArray{
            QJsonObject{{"full_name", fullName}, {"game_mask_id", "chosen-id"}},
            QJsonObject{{"full_name", "malformed-without-id"}}}}})};
    };
    const auto result = SearchStreamlabsCategories("dummy", fullName, cancelled);
    Check(sentQuery == fullName.left(25), "API receives only the original app's first 25 search characters");
    Check(result.categories.size() == 2 && result.categories[0].name == fullName && result.categories[0].id == "chosen-id",
        "Full category names and IDs survive search truncation; malformed rows are skipped");
    Check(result.categories.last().name == "Other" && result.categories.last().id.isEmpty(), "Other matches the original app");
    const QString unicode = QString::fromUtf8("🎮").repeated(30);
    SearchStreamlabsCategories("dummy", unicode, cancelled);
    Check(sentQuery == QString::fromUtf8("🎮").repeated(25), "Unicode characters are never truncated mid-codepoint or mid-UTF-8 byte");
    SearchStreamlabsCategories("dummy", "Music + games & fun", cancelled);
    Check(sentQuery == "Music + games & fun", "Query punctuation is percent-encoded safely");
    status = 429;
    Check(SearchStreamlabsCategories("dummy", "game", cancelled).errorKey, "Category errors are surfaced, not treated as empty success");
    const auto before = calls;
    SearchStreamlabsCategories("dummy", "", cancelled);
    SearchStreamlabsCategories("dummy", "Other", cancelled);
    cancelled = true;
    SearchStreamlabsCategories("dummy", "game", cancelled);
    Check(calls == before, "Empty, Other, and cancelled searches make no API request");
}

void DualFixtures()
{
    TikTokStudioAccountCredentials account;
    account.rapidapi_key = "fixture"; account.device_id = "11"; account.install_id = "12";
    account.cookie_jar = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tfixture\n";
    QJsonValue multi = true, allowed = true, used = false;
    QJsonValue success = 0;
    bool existing = false, created = false, lost = false;
    int creates = 0;
    QJsonObject room{{"id_str", "9001"}, {"stream_id_str", "9002"}, {"owner_user_id_str", "9003"},
        {"stream_url", QJsonObject{{"rtmp_push_url", "rtmp://fixture/live/portrait"}}},
        {"multi_stream_url", QJsonObject{{"rtmp_push_url", "rtmp://fixture/live/landscape"}}}};
    tiktok_http_fixture = [&](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray& body) -> TikTokHttpFixtureResponse {
        const auto path = url.path();
        if (path == "/get_domains/v4/") return {200, "{}"};
        if (path == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"gb"}})"};
        if (path == "/signatures") return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"};
        if (path == "/webcast/game/basic/create_info/") {
            Check(QUrlQuery(url).queryItemValue("scene") == "1", "Eligibility uses scene=1");
            return {200, Json({{"status_code", success}, {"data", QJsonObject{{"allow_multi_stream", multi}}}})};
        }
        if (path == "/webcast/anchor_tool/threshold/check/")
            return {200, Json({{"status_code", 0}, {"data", QJsonObject{{"allowed", allowed}, {"dual_canvas_used", used}, {"days_to_reach", 3}}}})};
        if (path == "/webcast/room/continue/") return {200, Json({{"data", (existing || created) ? QJsonObject{{"room", room}} : QJsonObject{}}})};
        if (path == "/webcast/room/create/") {
            ++creates; created = true;
            const QUrlQuery form(QString::fromUtf8(body));
            Check(form.queryItemValue("multi_stream_scene") == "1" && form.queryItemValue("multi_stream_source") == "1" &&
                form.queryItemValue("live_studio") == "1", "Both native clients explicitly request two feeds");
            if (lost) return {0, {}, "lost create reply"};
            return {200, Json({{"data", room}})};
        }
        if (path == "/webcast/room/ping/anchor/" || path == "/webcast/room/anchor_pre_finish/") return {200, "{\"status_code\":0}"};
        throw std::runtime_error("Unexpected dual fixture endpoint");
    };
    Check(check_tiktok_dual_access(account).isEmpty(), "Eligible dual account accepted");
    success = QJsonValue(QJsonValue::Undefined);
    Check(!lookup_tiktok_dual_access(account).available.has_value(), "Missing response success status cannot unlock dual");
    success = 1;
    Check(!lookup_tiktok_dual_access(account).available.has_value(), "Unsuccessful response cannot unlock dual");
    success = 0;
    allowed = false; Check(lookup_tiktok_dual_access(account).available == false && check_tiktok_dual_access(account).contains("25 minutes") &&
        check_tiktok_dual_access(account).contains("48 hours"), "Locked access explains remaining 25-minute sessions and validation delay like the original app");
    used = true; Check(check_tiktok_dual_access(account).isEmpty(), "Prior dual access accepted");
    multi = "false"; Check(!check_tiktok_dual_access(account).isEmpty(), "String false never enables dual access");
    multi = "unknown"; Check(check_tiktok_dual_access(account).contains("verify"), "Malformed eligibility stays unknown");
    multi = true; allowed = true; used = false;
    QString error;
    for (bool mobile : {false, true}) for (bool recover : {false, true}) {
        account.mobile = mobile; account.device_profile = {{"broadcast_type", "video"}};
        created = false; lost = recover; creates = 0;
        const auto live = create_tiktok_live(account, "Dual test", "5", "1", false, &error, true);
        Check(error.isEmpty() && live.key == "portrait" && live.dual_key == "landscape" && live.created_by_us && creates == 1,
            "New/recovered desktop/mobile room maps portrait and landscape and tracks ownership");
    }
    existing = true; created = false; creates = 0; account.mobile = false;
    auto live = create_tiktok_live(account, "Dual test", "5", "1", false, &error, true);
    Check(error.isEmpty() && !live.created_by_us && creates == 0, "Resume does not claim creation ownership");
    room.remove("multi_stream_url");
    live = create_tiktok_live(account, "Dual test", "5", "1", false, &error, true);
    Check(!error.isEmpty() && !live.created_by_us && creates == 0, "Existing single room rejected without recreation or ownership");
    room["multi_stream_url"] = room["stream_url"];
    live = create_tiktok_live(account, "Dual test", "5", "1", false, &error, true);
    Check(!error.isEmpty(), "Identical endpoints cannot start dual outputs");
    account.mobile = true; account.device_profile = {{"broadcast_type", "screen_record"}};
    live = create_tiktok_live(account, "Dual test", "5", "1", false, &error, true);
    Check(error.contains("Camera") && creates == 0, "Mobile screen capture rejects dual");
}

void RoutingAndErrorFixtures()
{
    const auto cookie = [](const char* name, const char* value) { return QByteArray(".tiktok.com\tTRUE\t/\tTRUE\t0\t") + name + '\t' + value + '\n'; };
    TikTokStudioAccountCredentials account;
    account.cookie_jar = cookie("store-idc", "alisg") + cookie("store-country-code", "fr");
    Check(tiktok_host_candidates(account, false, {}) == QStringList{"webcast16-normal-c-alisg.tiktokv.com", "webcast16-normal-no1a.tiktokv.eu"}, "Authenticated Singapore IDC wins over French store/priority region");
    account.cookie_jar = cookie("store-idc", "useast8");
    Check(tiktok_authenticated_host(account, true) == "api16-normal-useast8.tiktokv.us", "US IDC gets US domain suffix");
    account.cookie_jar = cookie("store-idc", "no1a");
    Check(tiktok_authenticated_host(account, false) == "webcast16-normal-no1a.tiktokv.eu", "European IDC gets EU domain suffix");
    account.cookie_jar = cookie("store-country-code", "fr");
    Check(tiktok_host_candidates(account, false, {}).last() == "webcast16-normal-no1a.tiktokv.eu", "Missing IDC retains regional fallback last");
    const auto payload = QJsonDocument::fromJson(R"({"data":{"ttnet_dispatch_actions":[
        {"act_priority":20,"param":{"strategy_info":{"webcast-normal.tiktokv.com":"webcast16-normal-no1a.tiktokv.eu"}}},
        {"act_priority":0,"param":{"strategy_info":{"webcast-normal.tiktokv.com":["webcast19.tiktokv.com"]}}},
        {"act_priority":1,"param":{"strategy_info":{"webcast-normal.tiktokv.com":"webcast.tiktokv.com","webcast.tiktokv.com":"webcast16-normal-alisg.tiktokv.com","webcast16-normal.tiktokv.com":"webcast.tiktokv.com","api.tiktokv.com":"api16-normal-alisg.tiktokv.com","log.tiktokv.com":"log16-normal-alisg.tiktokv.com","malicious.tiktokv.com":"https://outside.example/","cycle1.tiktokv.com":"cycle2.tiktokv.com","cycle2.tiktokv.com":"cycle1.tiktokv.com"}}}
    ]}})").object();
    const auto maps = tiktok_dispatch_maps(payload);
    Check(maps.size() == 2 && tiktok_host_candidates(account, false, maps).front() == "webcast16-normal-c-alisg.tiktokv.com", "Dispatch obeys action priority, ignores candidate arrays, follows chains and normalizes aliases");
    Check(tiktok_resolve_host("malicious.tiktokv.com", maps) == "malicious.tiktokv.com" && tiktok_resolve_host("cycle1.tiktokv.com", maps) == "cycle2.tiktokv.com", "Invalid destinations and cyclic dispatch are bounded");
    Check(tiktok_resolve_host("log.tiktokv.com", maps) == "log16-normal-alisg.tiktokv.com", "Desktop device-registration hosts also follow dispatch");
    const auto desktopKey = tiktok_routing_cache_key(account); account.mobile = true;
    Check(tiktok_routing_cache_key(account) != desktopKey, "Android never reuses desktop routing/region cache");
    const auto mobileRegionKey = tiktok_region_cache_key(account);
    account.device_id = "999"; account.install_id = "998";
    Check(tiktok_region_cache_key(account) == mobileRegionKey, "Region survives automatic device registration for the same login");
    account.mobile = false;
    Check(tiktok_region_cache_key(account) != mobileRegionKey, "Desktop and Android have separate region discovery");
    account.mobile = true; account.cookie_jar += cookie("sessionid", "different-login");
    Check(tiktok_region_cache_key(account) != mobileRegionKey, "A new login never reuses another session's region");

    for (bool mobile : {false, true}) {
        account = {}; account.mobile = mobile; account.rapidapi_key = "routing-key";
        account.device_id = "991"; account.install_id = "992";
        account.device_profile = {{"app_version", "47.1.4"}, {"request_params", QJsonObject{{"priority_region", "us"}}}};
        account.cookie_jar = cookie("sessionid", mobile ? "routing-mobile" : "routing-studio") + cookie("store-country-code", "fr");
        int dispatches = 0, regions = 0, creates = 0, continuations = 0;
        bool rejectAccount = false, rejectDual = false, httpFailure = false, wrongHost = false, rejectSigner = false;
        QString signedUrl;
        tiktok_http_fixture = [&](const QByteArray& method, const QUrl& url, const TikTokHttpHeaders& headers, const QByteArray& body) -> TikTokHttpFixtureResponse {
            if (url.path() == "/get_domains/v4/") {
                ++dispatches; const QUrlQuery query(url);
                Check(query.queryItemValue("aid") == (mobile ? "1233" : "8311") && query.queryItemValue("device_platform") == (mobile ? "android" : "win"), "TNC gets client-specific app and platform");
                if (mobile) Check(query.queryItemValue("iid") == "992" && query.queryItemValue("version_code") == "470104", "Android dispatch gets its own install identity and version");
                else Check(query.queryItemValue("tnc_src") == "6" && query.queryItemValue("version_code") == "1.27.0", "Desktop dispatch gets LIVE Studio version");
                return {200, Json(payload)};
            }
            if (url.path() == "/signatures") {
                if (rejectSigner) return {403, R"({"message":"Forbidden","data":{"message":"Subscription has expired for this signer"}})"};
                signedUrl = QJsonDocument::fromJson(body).object().value("url").toString();
                return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"};
            }
            if (mobile) Check(signedUrl == url.toString(QUrl::FullyEncoded), "Android signs the resolved URL actually sent");
            if (url.path() == "/passport/account/info/v2/") {
                if (wrongHost && url.host() == "api16-normal-c-alisg.tiktokv.com") return {421, R"({"message":"Wrong IDC"})"};
                Check(url.host() == (wrongHost ? "api16-normal-no1a.tiktokv.eu" : "api16-normal-c-alisg.tiktokv.com"), "Account uses dispatch result and can retry wrong-IDC transport failures");
                return {200, rejectAccount ? QByteArray(R"({"data":{"error_code":7,"description":"Cookie login expired"}})") : QByteArray(R"({"data":{"username":"route_account","region":"gb"}})")};
            }
            if (url.host() != "webcast16-normal-c-alisg.tiktokv.com") {
                Check(httpFailure || url.path() == "/webcast/room/continue/", "Only transport failures or continuation probes allow the regional fallback");
                return {503, R"({"message":"Fallback service unavailable"})"};
            }
            const QUrlQuery query(url);
            if (url.path() == "/webcast/game/basic/region_check/") {
                ++regions;
                Check(query.queryItemValue("priority_region") == (mobile ? "us" : ""), "Region bootstrap uses captured Android priority or blank Studio priority, never store country");
                return {200, R"({"status_code":0,"data":{"priority_region":"fr","store_region":"tn"}})"};
            }
            Check(query.queryItemValue("priority_region") == "fr" && headers.contains({"X-TT-Store-Region", "gb"}), "Discovered priority and account store region stay separate");
            if (url.path() == "/webcast/room/continue/") { ++continuations; return {200, R"({"status_code":0,"data":{}})"}; }
            if (url.path() == "/webcast/room/create/") {
                ++creates; Check(method == "POST", "Room creation uses POST");
                return httpFailure ? TikTokHttpFixtureResponse{403, R"({"message":"Request failed","data":{"prompts":"Account cannot create LIVE here"}})"}
                    : TikTokHttpFixtureResponse{200, R"({"status_code":4003019,"message":"Request failed","data":{"prompts":"LIVE restricted for this account"}})"};
            }
            if (url.path() == "/webcast/game/basic/create_info/" && rejectDual) return {400, R"({"message":"Bad request","data":{"message":"Dual access denied by account policy"}})"};
            throw std::runtime_error("Unexpected routing fixture endpoint");
        };
        QString error;
        Check(lookup_tiktok_username(account, &error) == "route_account" && error.isEmpty(), "Account lookup discovers routing and region");
        Check(dispatches == 1 && regions == 1, "Domain and region discovery cached for one client/session");
        account.device_id = "993"; // Simulate device setup between the account check and Start.
        auto live = create_tiktok_live(account, "fixture", "6", "0", false, &error);
        Check(error.contains("LIVE restricted for this account") && error.contains("4003019") && creates == 1 && continuations == 1 && live.room_id.isEmpty(), "Exact application rejection reaches native stream startup without another create or room cleanup");
        httpFailure = true;
        live = create_tiktok_live(account, "fixture", "6", "0", false, &error);
        Check(error.contains("Account cannot create LIVE here") && error.contains("403"), "Nested rejection is preserved on HTTP failure as well as HTTP 200");
        rejectDual = true;
        Check(lookup_tiktok_dual_access(account).reason.contains("Dual access denied by account policy"), "Eligibility UI keeps specific API failure");
        rejectAccount = true;
        Check(lookup_tiktok_username(account, &error).isEmpty() && error.contains("Cookie login expired"), "Passport nested error detail stays visible");
        rejectAccount = false; wrongHost = true;
        Check(lookup_tiktok_username(account, &error) == "route_account" && error.isEmpty(), "Wrong account-host transport response retries supported fallback");
        Check(dispatches == 2 && regions == 1, "Device changes refresh dispatch but preserve the same login's region context");
        rejectSigner = true;
        Check(lookup_tiktok_username(account, &error).isEmpty() && error.contains("Subscription has expired for this signer"), "Desktop and Android retain signer rejection instead of missing-signature errors");
    }
    const auto signer = RapidApiRequestSigner::parse_response(R"({"message":"Forbidden","data":{"message":"Subscription has expired for this signer"}})", 403);
    Check(signer.error.contains("Subscription has expired for this signer"), "RapidAPI guidance retains the actual service rejection");
    Check(tiktok_api_message({{"message", "Error"}, {"data", QJsonObject{{"status_msg", "Provider-specific reason"}}}}) == "Provider-specific reason", "Alternative native status-message field supported");
}

void StreamlabsErrorFixtures()
{
    const auto response = QJsonDocument::fromJson(R"({"message":"An unexpected error occurred when trying to connect to the server","data":{"message":"user already has a live room in TikTok Platform"}})").object();
    Check(StreamlabsApiError(response) == "user already has a live room in TikTok Platform", "Display TikTok rejection instead of generic Streamlabs wrapper");
    Check(StreamlabsApiError({{"data", QJsonObject{{"message", " Another provider rejection "}}}}) == "Another provider rejection", "Detailed messages do not require an outer message");
    for (auto data : {QJsonValue(QJsonObject{{"message", "  "}}), QJsonValue(QJsonObject{{"message", 42}}), QJsonValue("invalid"), QJsonValue(QJsonValue::Null)})
        Check(StreamlabsApiError({{"message", "Token expired"}, {"data", data}}) == "Token expired", "Missing or malformed details preserve the outer API error");
    Check(StreamlabsApiError({{"message", "  "}, {"data", QJsonObject{}}}) == "Unknown error occurred", "An absent API explanation has a nonempty fallback");
}

void LoginFixtures()
{
    TikTokStudioAccountCredentials account;
    account.device_id = "101"; account.install_id = "102"; account.rapidapi_key = "fixture";
    int requests = 0, qrRequests = 0;
    QString secret;
    QString mode = "new";
    const QByteArray goodCookie = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tqr-good-session\n";
    const QByteArray wrongCookie = ".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tqr-wrong-session\n";
    const QString qrUrl = "https://www.tiktok.com/login?next_url=" + QString::fromUtf8(QUrl::toPercentEncoding("https://www.tiktok.com/confirm?token=fixture"));
    const auto bound = bind_tiktok_qr_client_secret(qrUrl, "TEST1234");
    const QUrl next(QUrlQuery(QUrl(bound)).queryItemValue("next_url", QUrl::FullyDecoded));
    Check(QUrlQuery(next).queryItemValue("client_secret") == "TEST1234" && QUrlQuery(next).queryItemValue("token") == "fixture", "QR binding preserves nested query and injects this login's secret");
    tiktok_http_fixture = [&](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray&) -> TikTokHttpFixtureResponse {
        ++requests;
        if (url.path() == "/get_domains/v4/") return {200, R"({"data":{"ttnet_dispatch_actions":[{"param":{"strategy_info":{"api.tiktokv.com":"api16-normal-c-alisg.tiktokv.com"}}}]}})"};
        if (url.path() == "/signatures") return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a"})"};
        const QUrlQuery query(url);
        Check(query.queryItemValue("aid") == "8311" && !query.queryItemValue("sign").isEmpty(), "QR uses signed LIVE Studio passport parameters");
        if (url.path() == "/passport/web/get_qrcode/") {
            ++qrRequests;
            if (mode == "rejected") return {200, R"({"data":{"error_code":7,"description":"QR login is unavailable for this account"}})"};
            return {200, Json({{"data", QJsonObject{{"token", "qr-token"}, {"qrcode_index_url", qrUrl}}}})};
        }
        Check(url.path() == "/passport/web/check_qrconnect/" && query.queryItemValue("token") == "qr-token", "QR poll uses the returned token and never starts a LIVE");
        if (mode == "confirmed" || mode == "wrong-secret" || mode == "no-cookies") {
            const bool correct = url.host() == "api16-normal-no1a.tiktokv.eu" && mode != "wrong-secret";
            return {200, Json({{"data", QJsonObject{{"status", "confirmed"}, {"client_secret", correct ? secret : "wrong"}, {"username", "qr_fixture"}}}}), {},
                mode == "no-cookies" && correct ? QByteArray() : correct ? goodCookie : wrongCookie};
        }
        return {200, Json({{"data", QJsonObject{{"status", mode}, {"client_secret", secret}}}})};
    };
    auto login = begin_tiktok_qr_login(account);
    secret = login.client_secret;
    Check(login.error.isEmpty() && secret.size() == 8 && !QImage::fromData(login.png, "PNG").isNull(), "Native QR login returns a locally rendered readable QR image");
    Check(poll_tiktok_qr_login(login).state == TikTokQrState::Waiting, "Unscanned QR stays pending");
    mode = "scanned";
    Check(poll_tiktok_qr_login(login).state == TikTokQrState::Scanned, "Scanned QR waits for phone confirmation");
    mode = "expired";
    Check(poll_tiktok_qr_login(login).state == TikTokQrState::Expired, "Expired QR requires regeneration");
    mode = "wrong-secret";
    auto wrong = poll_tiktok_qr_login(login);
    Check(wrong.state != TikTokQrState::Confirmed && wrong.account.cookie_jar.isEmpty(), "Wrong-secret confirmations never import cookies");
    mode = "no-cookies";
    Check(poll_tiktok_qr_login(login).error.contains("did not return login cookies"), "Other IDC cookies cannot satisfy a cookie-less confirmation");
    mode = "confirmed";
    auto confirmed = poll_tiktok_qr_login(login);
    Check(confirmed.error.isEmpty() && confirmed.state == TikTokQrState::Confirmed && confirmed.host == "api16-normal-no1a.tiktokv.eu" &&
        confirmed.account.cookie_jar.contains("qr-good-session") && !confirmed.account.cookie_jar.contains("qr-wrong-session"), "Only matching confirmed IDC exports its session");
    QTemporaryDir directory;
    QString error;
    const auto file = save_tiktok_login_cookies(confirmed.account.cookie_jar, directory.path(), &error, confirmed.account.username);
    Check(!file.isEmpty() && error.isEmpty() && read_tiktok_cookie_file(file, &error).contains("qr-good-session"), "QR cookies round-trip into normal desktop/mobile inputs");
    Check(QFileInfo(file).fileName() == "cookies_qr_fixture.json", "Cookie export uses the account username and JSON extension");
    QFile exported(file); Check(exported.open(QIODevice::ReadOnly), "Read exported cookie JSON");
    const auto originalBytes = exported.readAll(); exported.close();
    Check(QJsonDocument::fromJson(originalBytes).isArray(), "JSON extension contains a browser-compatible cookie array");
    const auto second = save_tiktok_login_cookies(confirmed.account.cookie_jar, directory.path(), &error, confirmed.account.username);
    Check(second != file && QFile::exists(file) && QFileInfo(second).fileName().startsWith("cookies_qr_fixture_"),
        "Repeated logins add a suffix without overwriting the existing username file");
    Check(exported.open(QIODevice::ReadOnly) && exported.readAll() == originalBytes, "Repeated login preserves original cookie contents"); exported.close();
    const auto unsafeName = save_tiktok_login_cookies(confirmed.account.cookie_jar, directory.path(), &error, "@../outside\\file:name");
    Check(!unsafeName.isEmpty() && QFileInfo(unsafeName).absolutePath() == directory.path() && unsafeName.endsWith(".json"),
        "Account names cannot escape the cookie directory");
    const QByteArray attributes = "#HttpOnly_.tiktok.com\tTRUE\t/live\tTRUE\t1900000000\tsessionid\tquoted-\"cookie\n"
        ".unrelated.example\tTRUE\t/\tTRUE\t0\tprivate\tnot-exported\n";
    const auto attributeFile = save_tiktok_login_cookies(attributes, directory.path(), &error, "@cookie.attributes");
    QFile exportedAttributes(attributeFile); Check(exportedAttributes.open(QIODevice::ReadOnly), "Read cookie attributes JSON");
    const auto cookieArray = QJsonDocument::fromJson(exportedAttributes.readAll()).array();
    const auto cookie = cookieArray.first().toObject();
    Check(cookieArray.size() == 1 && cookie.value("domain") == ".tiktok.com" && cookie.value("path") == "/live" &&
        cookie.value("httpOnly").toBool() && cookie.value("secure").toBool() && cookie.value("expires").toInteger() == 1900000000 &&
        cookie.value("value") == "quoted-\"cookie", "Cookie JSON preserves attributes/escaping and excludes unrelated domains");
    mode = "rejected";
    Check(begin_tiktok_qr_login(account).error.contains("QR login is unavailable for this account"), "QR failures preserve the provider's detailed explanation");
    const int before = requests;
    { std::atomic_bool cancelled{true}; TikTokRequestCancellation scope(&cancelled); poll_tiktok_qr_login(login); }
    Check(before == requests && qrRequests == 2, "Cancellation stops QR polling requests");

    auto writeLog = [&](const QString& name, QByteArray bytes, int seconds) {
        QFile log(directory.filePath(name)); Check(log.open(QIODevice::WriteOnly), "Create local detection fixture");
        log.write(bytes); log.setFileTime(QDateTime::currentDateTime().addSecs(seconds), QFileDevice::FileModificationTime); log.close();
    };
    writeLog("old.log", R"({"apiToken":"1111"})", -120);
    QByteArray latest(1024 * 1024 - 10, 'x'); latest += R"({"apiToken":"2222"}{"apiToken":"aBcD1234"})";
    QByteArray nulRecord;
    for (char c : latest.right(26)) { nulRecord += c; nulRecord += '\0'; }
    writeLog("new.log", latest + nulRecord, -60);
    writeLog("empty.log", "no token here", 0);
    std::atomic_bool cancelled{false};
    Check(FindStreamlabsDesktopToken(directory.path(), cancelled) == "aBcD1234", "Desktop detection uses newest readable log's last token across chunks and NUL bytes");
    cancelled = true;
    Check(FindStreamlabsDesktopToken(directory.path(), cancelled).isEmpty(), "Cancelled detection never fills credentials");
}

void QrRecoveryFixtures()
{
    TikTokStudioAccountCredentials account;
    account.device_id = "401"; account.install_id = "402"; account.rapidapi_key = "fixture";
    int signs = 0, polls = 0, generations = 0, failAfter = 0;
    long signerStatus = 200, pollStatus = 200;
    CURLcode signerCode = CURLE_OK, pollCode = CURLE_OK;
    QString status = "new", secret;
    bool staleRegion = false;
    tiktok_http_fixture = [&](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray&) -> TikTokHttpFixtureResponse {
        if (url.path() == "/get_domains/v4/") return {200, R"({"data":{"ttnet_dispatch_actions":[{"param":{"strategy_info":{"api.tiktokv.com":"api16-normal-c-alisg.tiktokv.com"}}}]}})"};
        if (url.path() == "/signatures") {
            ++signs;
            if (signs > failAfter && signerCode != CURLE_OK) return {0, {}, {}, {}, {}, signerCode};
            if (signs > failAfter && signerStatus != 200) return {signerStatus, R"({"message":"Fixture signer rejection"})"};
            return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a"})"};
        }
        if (url.path() == "/passport/web/get_qrcode/") {
            ++generations;
            return {200, R"({"data":{"token":"recovery-token","qrcode_index_url":"https://www.tiktok.com/login?next_url=https%3A%2F%2Fwww.tiktok.com%2Fconfirm"}})"};
        }
        Check(url.path() == "/passport/web/check_qrconnect/", "Recovery only checks the QR login");
        ++polls;
        if (pollCode != CURLE_OK) return {0, {}, {}, {}, {}, pollCode};
        if (pollStatus != 200) return {pollStatus, R"({"message":"Fixture passport rejection"})"};
        const bool stale = staleRegion && url.host() != "api16-normal-no1a.tiktokv.eu";
        return {200, Json({{"data", QJsonObject{{"status", stale ? QString("new") : status}, {"client_secret", stale ? QString("stale-secret") : secret}}}}), {},
            status == "confirmed" ? QByteArray(".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\trecovered-session\n") : QByteArray()};
    };
    auto original = begin_tiktok_qr_login(account);
    secret = original.client_secret;
    Check(original.error.isEmpty(), "Recovery fixture begins with a working QR code");
    for (auto code : {CURLE_OPERATION_TIMEDOUT, CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT, CURLE_RECV_ERROR}) {
        signerCode = code; signs = polls = 0;
        auto failed = poll_tiktok_qr_login(original);
        Check(failed.retryable && failed.error.contains("signer could not be reached") && signs == 1 && polls == 0,
            "Temporary signer failure schedules recovery without immediate regional signer retries");
        Check(failed.token == original.token && failed.png == original.png && failed.client_secret == secret,
            "Temporary failure preserves the same QR image, token and secret");
        signerCode = CURLE_OK;
        auto recovered = poll_tiktok_qr_login(failed);
        Check(recovered.error.isEmpty() && !recovered.retryable && recovered.state == TikTokQrState::Waiting,
            "A later successful poll clears the transient error and retry state");
    }
    for (long http : {408, 500, 502, 503, 504, 401, 403, 429}) {
        signerStatus = http; signs = polls = 0;
        auto failed = poll_tiktok_qr_login(original);
        Check(failed.retryable == (http == 408 || http >= 500) && !failed.error.isEmpty() && signs == 1 && polls == 0,
            "Only temporary HTTP errors retry; key, subscription and quota rejections stop");
    }
    Check(RapidApiRequestSigner::parse_response("<html>Bad gateway</html>", 502).transient,
        "HTML gateway errors are classified before JSON parsing");
    signerStatus = 200; signerCode = CURLE_PEER_FAILED_VERIFICATION;
    Check(!poll_tiktok_qr_login(original).retryable, "TLS certificate errors are not treated as a temporary outage");
    signerCode = CURLE_ABORTED_BY_CALLBACK;
    Check(!poll_tiktok_qr_login(original).retryable, "Cancelled signer calls never retry");
    signerCode = CURLE_OPERATION_TIMEDOUT; failAfter = 1; signs = 0;
    auto partial = poll_tiktok_qr_login(original);
    Check(partial.error.isEmpty() && partial.state == TikTokQrState::Waiting && signs == 2,
        "A later regional signer failure cannot discard a valid pending response");
    signerCode = CURLE_OK; failAfter = 0; pollCode = CURLE_OPERATION_TIMEDOUT;
    auto interrupted = poll_tiktok_qr_login(original);
    Check(interrupted.retryable && !interrupted.error.isEmpty(), "Temporary TikTok polling outages can recover too");
    pollCode = CURLE_OK; pollStatus = 503;
    Check(poll_tiktok_qr_login(original).retryable, "TikTok gateway outages retain a retryable login");
    pollStatus = 200; status = "scanned";
    auto scanned = poll_tiktok_qr_login(interrupted);
    Check(scanned.error.isEmpty() && !scanned.retryable && scanned.state == TikTokQrState::Scanned,
        "Scanning during an outage is detected after recovery");
    signerCode = CURLE_OPERATION_TIMEDOUT;
    interrupted = poll_tiktok_qr_login(scanned);
    Check(interrupted.state == TikTokQrState::Scanned, "An outage preserves phone confirmation state");
    signerCode = CURLE_OK; status = "confirmed";
    auto confirmed = poll_tiktok_qr_login(interrupted);
    Check(confirmed.error.isEmpty() && confirmed.state == TikTokQrState::Confirmed &&
        confirmed.account.cookie_jar.contains("recovered-session") && generations == 1,
        "Confirmation after recovery exports cookies without regenerating the QR code");
    status = "expired";
    auto expired = poll_tiktok_qr_login(interrupted);
    Check(expired.error.isEmpty() && !expired.retryable && expired.state == TikTokQrState::Expired,
        "Expiry after an outage is terminal");
    staleRegion = true;
    Check(poll_tiktok_qr_login(original).state == TikTokQrState::Expired,
        "A matching expired response outranks another region's stale new code");
    for (const auto terminal : {QString("refuse"), QString("refused"), QString("error")}) {
        status = terminal;
        const auto result = poll_tiktok_qr_login(original);
        Check(result.error.isEmpty() && result.state == (terminal == "error" ? TikTokQrState::Error : TikTokQrState::Refused),
            "Refused and error states request a replacement rather than polling the old token");
    }
    const int before = signs + polls;
    { std::atomic_bool cancelled{true}; TikTokRequestCancellation scope(&cancelled); poll_tiktok_qr_login(interrupted); }
    Check(signs + polls == before, "Cancellation prevents all recovery requests");
    std::cout << "PASS: QR outage recovery, terminal rejections, regional responses and confirmation\n";
}

void LiveSettingsFixtures()
{
    for (bool mobile : {false, true}) for (bool gifts : {false, true}) for (bool comments : {false, true})
        for (const auto disclosure : {"off", "own_brand", "third_party", "both"})
            LiveFixtures(mobile, false, false, "6", "video", "1", {gifts, comments, disclosure});
    for (bool mobile : {false, true}) for (bool resume : {false, true}) {
        TikTokStudioAccountCredentials account;
        account.mobile = mobile; account.device_id = "101"; account.install_id = "102"; account.rapidapi_key = "fixture";
        account.cookie_jar = ".tiktok.com\tTRUE\t/\tTRUE\t0\tstore-idc\tc-alisg\n.tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tsettings-fixture\n";
        const QJsonObject room{{"id_str", "9001"}, {"stream_id_str", "9002"}, {"owner_user_id_str", "9003"},
            {"stream_url", QJsonObject{{"rtmp_push_url", "rtmp://fixture/live/key"}}}, {"room_auth", QJsonObject{{"Gift", "1"}, {"Chat", true}}}};
        int creates = 0, prepares = 0;
        tiktok_http_fixture = [&](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray& body) -> TikTokHttpFixtureResponse {
            const auto path = url.path();
            if (path == "/signatures") return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"};
            if (path == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"gb"}})"};
            if (path == "/webcast/room/continue/") return {200, Json({{"status_code", 0}, {"data", resume ? QJsonObject{{"room", room}} : QJsonObject{}}})};
            if (path == "/webcast/room/create/") { ++creates; return {200, Json({{"status_code", 0}, {"data", room}})}; }
            if (path == "/webcast/room/ping/anchor/") { if (QUrlQuery(QString::fromUtf8(body)).queryItemValue("status") == "1") ++prepares; return {200, "{}"}; }
            throw std::runtime_error("Unexpected LIVE settings fixture endpoint");
        };
        QString error;
        auto live = create_tiktok_live(account, "fixture", "6", "0", false, &error, false, {false, false, "both"});
        if (resume) Check(creates == 0 && prepares == 1 && error.isEmpty() && !live.created_by_us, "Resuming keeps existing room settings without attempting an update or new room");
        else Check(creates == 1 && prepares == 0 && live.created_by_us && error.contains("gifts and comments"), "Explicit server contradictions fail before preparation while retaining ownership for cleanup");
        creates = 0;
        live = create_tiktok_live(account, "fixture", "6", "0", false, &error, false, {true, true, "invalid"});
        Check(!error.isEmpty() && creates == 0 && live.room_id.isEmpty(), "Invalid disclosure is rejected before creating a room");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--check-cookie-file") {
        QString error;
        const auto jar = read_tiktok_cookie_file(app.arguments()[2], &error);
        if (!error.isEmpty() || TikTokSafetyCookieIdentity(jar).isEmpty()) {
            std::cerr << "FAIL: cookie import did not produce a login session\n"; return 1;
        }
        std::cout << "PASS: cookie file imports with a login session; no network request made\n"; return 0;
    }
    try {
        CookieAndDeviceFixtures();
        MobileDeviceFixtures();
        DualFixtures();
        for (bool mobile : {false, true}) {
            LiveFixtures(mobile, false, false);
            LiveFixtures(mobile, true, false);
            LiveFixtures(mobile, false, true);
        }
        SigningFixtures();
        QuotaFixtures();
        LiveSafetyFixtures();
        LiveFixtures(false, false, false, "6");
        LiveFixtures(true, false, false, "6");
        LiveFixtures(true, false, false, "5", "screen_record", "1");
        LiveFixtures(true, false, false, "5", "screen_record", "2");
        LiveFixtures(true, false, false, "6", "screen_record", "2");
        GameCatalogueFixtures();
        AccountFixtures();
        CategoryFixtures();
        StreamlabsErrorFixtures();
        RoutingAndErrorFixtures();
        LoginFixtures();
        QrRecoveryFixtures();
        LiveSettingsFixtures();
        std::cout << "PASS: cookies, device envelope, desktop/mobile wire formats, create recovery, cleanup, and Android frame signatures\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
