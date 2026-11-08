#include "mobile_device.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#else
#include <openssl/evp.h>
#endif

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
QJsonObject object(const QJsonObject& parent, const char* name) {
    require(!parent.contains(name) || parent.value(name).isObject(), "Android profile fields must be objects.");
    return parent.value(name).toObject();
}
QByteArray random_bytes(int count) {
    QByteArray result(count, Qt::Uninitialized);
    for (auto& byte : result) byte = char(QRandomGenerator::system()->generate() & 255);
    return result;
}
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void merge(QJsonObject& to, const QJsonObject& from) {
    for (auto it = from.begin(); it != from.end(); ++it)
        if (!it.value().isNull() && it.value() != QJsonValue(QString())) to[it.key()] = it.value();
}
bool valid_id(const QString& value) {
    return QRegularExpression("^[0-9]+$").match(value).hasMatch() && value.contains(QRegularExpression("[1-9]"));
}
QByteArray aes_cbc(const QByteArray& raw, const QByteArray& key, QByteArray iv) {
    QByteArray result(raw.size() + 16, Qt::Uninitialized);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    ULONG length = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0) >= 0;
    if (ok) ok = BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())), ULONG(key.size()), 0) >= 0;
    if (ok) ok = BCryptEncrypt(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(raw.constData())), ULONG(raw.size()),
        nullptr, reinterpret_cast<PUCHAR>(iv.data()), ULONG(iv.size()), reinterpret_cast<PUCHAR>(result.data()),
        ULONG(result.size()), &length, BCRYPT_BLOCK_PADDING) >= 0;
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    require(ok, "Could not encrypt Android registration.");
    result.resize(length);
#elif defined(__APPLE__)
    size_t length = 0;
    require(CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.constData(), key.size(),
        iv.constData(), raw.constData(), raw.size(), result.data(), result.size(), &length) == kCCSuccess,
        "Could not encrypt Android registration.");
    result.resize(length);
#else
    auto context = EVP_CIPHER_CTX_new();
    require(context, "Could not create Android encryption context.");
    int length = 0, tail = 0;
    const bool ok = EVP_EncryptInit_ex(context, EVP_aes_128_cbc(), nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()), reinterpret_cast<const unsigned char*>(iv.constData())) == 1 &&
        EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(result.data()), &length,
            reinterpret_cast<const unsigned char*>(raw.constData()), int(raw.size())) == 1 &&
        EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char*>(result.data()) + length, &tail) == 1;
    EVP_CIPHER_CTX_free(context);
    require(ok, "Could not encrypt Android registration.");
    result.resize(length + tail);
#endif
    return result;
}
QByteArray gzip(const QByteArray& raw) {
    const auto compressed = qCompress(raw, 6);
    require(compressed.size() >= 10, "Could not compress Android registration.");
    QByteArray result = QByteArray::fromHex("1f8b0800000000000000");
    result += compressed.mid(6, compressed.size() - 10);
    quint32 crc = 0xffffffffu;
    for (const auto byte : raw) {
        crc ^= static_cast<unsigned char>(byte);
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    for (auto value : {~crc, quint32(raw.size())})
        for (int i = 0; i < 4; ++i) result += char(value >> (i * 8));
    return result;
}
}

QJsonObject complete_mobile_device_profile(QJsonObject profile) {
    const QList<QJsonObject> presets{
        {{"model", "Pixel 8"}, {"brand", "google"}, {"manufacturer", "Google"}, {"os_version", "14"}, {"os_api", "34"}, {"resolution", "2400x1080"}, {"dpi", "420"}},
        {{"model", "Pixel 7"}, {"brand", "google"}, {"manufacturer", "Google"}, {"os_version", "13"}, {"os_api", "33"}, {"resolution", "2400x1080"}, {"dpi", "420"}},
        {{"model", "SM-S911B"}, {"brand", "samsung"}, {"manufacturer", "samsung"}, {"os_version", "13"}, {"os_api", "33"}, {"resolution", "2340x1080"}, {"dpi", "420"}},
        {{"model", "CPH2583"}, {"brand", "OnePlus"}, {"manufacturer", "OnePlus"}, {"os_version", "14"}, {"os_api", "34"}, {"resolution", "3168x1440"}, {"dpi", "560"}}
    };
    auto saved = object(profile, "fingerprint");
    auto fingerprint = presets.at(QRandomGenerator::system()->bounded(presets.size()));
    for (const auto& preset : presets) if (preset.value("model") == saved.value("model")) fingerprint = preset;
    const QList<QJsonObject> zones{
        {{"timezone", "0"}, {"timezone_name", "Etc/UTC"}, {"tz_offset", 0}, {"region", "US"}},
        {{"timezone", "1"}, {"timezone_name", "Africa/Tunis"}, {"tz_offset", 3600}, {"region", "TN"}},
        {{"timezone", "9"}, {"timezone_name", "Asia/Tokyo"}, {"tz_offset", 32400}, {"region", "JP"}}
    };
    merge(fingerprint, zones.at(QRandomGenerator::system()->bounded(zones.size())));
    fingerprint["cpu_abi"] = "arm64-v8a"; fingerprint["language"] = "en";
    merge(fingerprint, saved);
    auto ids = object(profile, "identifiers");
    if (ids.value("android_id").toString().isEmpty()) ids["android_id"] = QString::fromLatin1(random_bytes(8).toHex());
    if (ids.value("openudid").toString().isEmpty()) ids["openudid"] = ids.value("android_id");
    for (const auto key : {"clientudid", "cdid"}) if (ids.value(key).toString().isEmpty()) ids[key] = uuid();
    profile["identifiers"] = ids; profile["fingerprint"] = fingerprint;
    if (!profile.contains("aid")) profile["aid"] = "1233";
    if (!profile.contains("apk_first_install_time")) profile["apk_first_install_time"] = QDateTime::currentMSecsSinceEpoch();
    return profile;
}

QByteArray encrypt_mobile_registration(const QByteArray& raw, QByteArray seed) {
    require(!raw.isEmpty() && raw.size() <= 256 * 1024, "Android encryption input must be 1..256 KiB.");
    if (seed.isEmpty()) seed = random_bytes(32);
    require(seed.size() == 32, "Android encryption seed must be 32 bytes.");
    const auto hash = [](const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha512); };
    const auto material = hash(hash(seed) + QByteArray::fromHex(
        "4dd4c2e6b83162090e52b3c7a6733ba41cb2462b829ab58a196b39db57177524f"
        "49baf7f08e8d68d26a72e37c1a95a2f1f05a51892aef2949732b62a38aadd58"));
    return QByteArray::fromHex("746305100000") + seed + aes_cbc(hash(raw) + raw, material.left(16), material.mid(16, 16));
}

MobileRegistrationRequest prepare_mobile_registration(const QJsonObject& profile, qint64 timestampMs, QString requestId, const QByteArray& seed) {
    const auto ids = object(profile, "identifiers"), fp = object(profile, "fingerprint");
    require(profile.value("aid").toVariant().toString() == "1233", "Android registration requires aid 1233.");
    require(QRegularExpression("^[0-9a-fA-F]{16}$").match(ids.value("android_id").toString()).hasMatch(), "Invalid Android ID.");
    for (const auto name : {"clientudid", "cdid"}) require(!QUuid(ids.value(name).toString()).isNull(), "Invalid Android installation UUID.");
    for (const auto name : {"model", "brand", "os_version", "os_api"}) require(!fp.value(name).toVariant().toString().trimmed().isEmpty(), "Incomplete Android hardware profile.");
    bool validApi = false;
    const int osApi = fp.value("os_api").toVariant().toString().toInt(&validApi);
    require(validApi && osApi >= 1 && osApi <= 100, "Invalid Android API level.");
    const auto version = profile.value("app_version").toString("47.1.4");
    require(QRegularExpression("^[0-9]+\\.[0-9]+\\.[0-9]+$").match(version).hasMatch(), "Invalid Android app version.");
    qint64 versionCode = 0;
    for (const auto& part : version.split('.')) { require(part.size() <= 3, "Invalid Android app version."); versionCode = versionCode * 100 + part.toInt(); }
    bool validBuild = true;
    const auto build = profile.contains("version_code") ? profile.value("version_code").toVariant().toString().toLongLong(&validBuild) : 2024701040;
    require(validBuild && build > 0, "Invalid Android build code.");
    if (timestampMs < 0) timestampMs = QDateTime::currentMSecsSinceEpoch();
    if (requestId.isEmpty()) requestId = uuid();
    require(!QUuid(requestId).isNull(), "Invalid Android registration request ID.");
    QJsonObject header{{"aid", 1233}, {"app_name", "musical_ly"}, {"package", "com.zhiliaoapp.musically"},
        {"channel", "googleplay"}, {"not_request_sender", 1}, {"app_version", version}, {"version_code", versionCode},
        {"update_version_code", build}, {"manifest_version_code", build}, {"device_platform", "android"},
        {"os", "Android"}, {"os_version", fp.value("os_version")}, {"os_api", osApi},
        {"device_model", fp.value("model")}, {"device_brand", fp.value("brand")},
        {"openudid", ids.value("openudid")}, {"clientudid", ids.value("clientudid")}, {"cdid", ids.value("cdid")},
        {"git_hash", "43cd1bf"}, {"sdk_version_code", 205140690}, {"sdk_target_version", 30}, {"sdk_version", "2.5.14.6"},
        {"sdk_flavor", "i18nInner"}, {"guest_mode", 0}, {"is_system_app", 0}, {"req_id", requestId},
        {"apk_first_install_time", profile.value("apk_first_install_time")}};
    for (const auto name : {"device_id", "install_id"}) {
        const auto value = profile.value(name).toVariant().toString();
        if (!value.isEmpty()) { require(valid_id(value), "Invalid Android device or install ID."); header[name] = value; }
    }
    const QList<QPair<QString, QString>> fields{{"manufacturer", "device_manufacturer"}, {"cpu_abi", "cpu_abi"},
        {"resolution", "resolution"}, {"dpi", "density_dpi"}, {"language", "language"}, {"timezone", "timezone"},
        {"tz_offset", "tz_offset"}, {"rom", "rom"}, {"rom_version", "rom_version"}};
    for (const auto& [from, to] : fields) if (fp.contains(from)) header[to] = fp.value(from);
    const auto registration = object(profile, "registration");
    if (registration.contains("ssid")) header["ssid"] = registration.value("ssid");
    QMap<QString, QString> query{{"aid", "1233"}, {"app_version", version}, {"req_id", requestId},
        {"device_platform", "android"}, {"version_code", QString::number(versionCode)}};
    if (header.contains("device_id")) query["device_id"] = header.value("device_id").toString();
    const auto options = object(profile, "registration_options");
    const auto extraHeader = object(options, "header"), extraQuery = object(options, "query");
    for (auto it = extraHeader.begin(); it != extraHeader.end(); ++it) {
        require(!header.contains(it.key()) || header.value(it.key()) == it.value(), "Android registration header conflicts with profile.");
        header[it.key()] = it.value();
    }
    for (auto it = extraQuery.begin(); it != extraQuery.end(); ++it) {
        const auto value = it.value().toVariant().toString();
        require((!query.contains(it.key()) || query.value(it.key()) == value) && it.key() != "tt_data" && it.key() != "tt_info", "Android registration query conflicts with profile.");
        query[it.key()] = value;
    }
    const auto host = options.value("host").toString("log-va.tiktokv.com"), encoding = options.value("encoding").toString("ttencrypt");
    require(QStringList{"log-va.tiktokv.com", "api.tiktokv.com", "aggr32-normal.tiktokv.com", "api-boot.tiktokv.com"}.contains(host), "Unsupported Android registration host.");
    require(QStringList{"ttencrypt", "gzip", "json-gzip", "json"}.contains(encoding), "Unsupported Android registration encoding.");
    const auto raw = QJsonDocument(QJsonObject{{"header", header}, {"magic_tag", "ss_app_log"}, {"_gen_time", timestampMs}}).toJson(QJsonDocument::Compact);
    require(raw.size() <= 32768, "Android registration payload exceeds 32 KiB.");
    MobileRegistrationRequest request;
    request.body = encoding == "json" ? raw : encoding == "ttencrypt" ? encrypt_mobile_registration(gzip(raw), seed) : gzip(raw);
    request.headers = {{"content-type", "application/json; charset=utf-8"}, {"accept-encoding", "gzip"},
        {"accept", "application/json"}, {"x-tt-request-tag", header.contains("device_id") && header.contains("install_id") ? "t=0;n=0" : "t=0;n=1"},
        {"x-ss-stub", QCryptographicHash::hash(encoding == "json-gzip" ? raw : request.body, QCryptographicHash::Md5).toHex()}};
    if (encoding == "gzip") { query["tt_data"] = "a"; request.headers[0].second = "application/octet-stream;tt-data=a"; }
    if (encoding == "json-gzip") request.headers.append(QPair<QByteArray, QByteArray>{"x-bd-content-encoding", "gzip"});
    if (encoding == "json" || encoding == "json-gzip") request.headers.append({"x-ss-req-ticket", QByteArray::number(timestampMs)});
    QUrlQuery params;
    for (auto it = query.begin(); it != query.end(); ++it) params.addQueryItem(it.key(), it.value());
    request.url = QUrl("https://" + host + "/service/2/device_register/"); request.url.setQuery(params);
    return request;
}

QJsonObject parse_mobile_registration(const QJsonObject& response) {
    const auto data = response.contains("data") ? object(response, "data") : response;
    QJsonObject result;
    for (const auto name : {QStringLiteral("device_id"), QStringLiteral("install_id")}) {
        auto value = data.value(name + "_str");
        if (value.isUndefined() || value.isNull() || value == QJsonValue(QString())) value = data.value(name);
        require(!value.isDouble() || value == QJsonValue(value.toInteger(-1)), "Android registration returned a non-integer ID.");
        const QString id = value.isString() ? value.toString().trimmed() : value.isDouble() ? QString::number(value.toInteger(-1)) : QString();
        require(valid_id(id), "TikTok did not issue valid Android device/install IDs. Refresh to retry, or enter valid IDs manually.");
        result[name] = id;
    }
    QJsonObject registration;
    for (const auto name : {"device_token", "ssid", "cd", "new_user"}) if (data.contains(name)) registration[name] = data.value(name);
    result["registration"] = registration;
    return result;
}
