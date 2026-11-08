#include "streamlabs-api.h"
#include "streamlabs-info.h"
#include "tiktok-native/request_cancellation.hpp"
#include <vector>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "obs.hpp"
#include "pch.h"

size_t StreamlabsAPI::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalBytes = size * nmemb;
    std::string* buf = static_cast<std::string*>(userp);
    if (totalBytes > 8 * 1024 * 1024 || buf->size() > 8 * 1024 * 1024 - totalBytes) return 0;
    buf->append(static_cast<char*>(contents), totalBytes);
    return totalBytes;
}

std::string StreamlabsAPI::ExtractStreamId(const std::string &key)
{
    const std::string prefix = "stream-";
    auto pos = key.find(prefix);
    if (pos == std::string::npos)
        return "";
    
    pos += prefix.size();
    
    std::string id;
    while (pos < key.size() && key[pos] != '?') {
        if (!isdigit(static_cast<unsigned char>(key[pos])))
            break;
        id.push_back(key[pos]);
        pos++;
    }
    return id;
}

bool StreamlabsAPI::EndStream(const std::string &token, const std::string &streamID)
{
    if (streamID.empty()) {
        blog(LOG_WARNING, TAG "EndStream: streamID is empty");
        return false;
    }

    std::string url = "https://streamlabs.com/api/v5/slobs/tiktok/stream/" + streamID + "/end";

    CURL* curl = curl_easy_init();
    if (!curl) {
        blog(LOG_WARNING, TAG "EndStream: curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);

    struct curl_slist* headers = nullptr;
    {
        std::string authHeader = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, authHeader.c_str());
        headers = curl_slist_append(
            headers,
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 "
            "Electron/29.3.1 Safari/537.36"
        );
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string responseData;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);

    CURLcode res = curl_easy_perform(curl);

    bool success = false;
    if (res == CURLE_OK) {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(responseData));
        if (!doc.isNull()) {
            QJsonObject obj = doc.object();
            success = obj.value("success").toBool(false);
            if (!success) {
                blog(LOG_WARNING, TAG "EndStream: JSON 'success' was false or missing");
            }
        } else {
            blog(LOG_WARNING, TAG "EndStream: Invalid JSON response: %s", responseData.c_str());
        }
    } else {
        blog(LOG_WARNING, TAG "EndStream: cURL error: %s", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return success;
}

std::tuple<bool, std::string, std::string, std::string> StreamlabsAPI::StartStream(const std::string& token, const std::string& title, const std::string& category, const int audienceType)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return {false, "Failed to initialize CURL", "", ""};
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, "https://streamlabs.com/api/v5/slobs/tiktok/stream/start");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 25000L);
    set_tiktok_request_cancellation(curl);
    
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, 
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "StreamlabsDesktop/1.17.0 Chrome/122.0.6261.156 "
        "Electron/29.3.1 Safari/537.36"
    );
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    curl_mime* mime = curl_mime_init(curl);
    auto addPart = [&](const char* fieldName, const char* value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, fieldName);
        curl_mime_data(part, value, CURL_ZERO_TERMINATED);
    };
    
    addPart("title", title.c_str());
    addPart("category", category.c_str());
    addPart("audience_type", std::to_string(audienceType).c_str());
    addPart("device_platform", "win32");
    
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    
    std::string responseData;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
    
    CURLcode res = curl_easy_perform(curl);
    
    bool success = false;
    std::string newServer, newKey, errorMessage;
    
    if (res == CURLE_OK) {
        QJsonDocument responseDoc = QJsonDocument::fromJson(
            QByteArray::fromStdString(responseData)
        );
        QJsonObject responseObj = responseDoc.object();

        if (responseObj.contains("rtmp") && responseObj.contains("key")) {
            newServer = responseObj["rtmp"].toString().toStdString();
            newKey = responseObj["key"].toString().toStdString();
            success = true;
        } else {
            errorMessage = StreamlabsApiError(responseObj).toStdString();
            blog(LOG_WARNING, TAG "Streamlabs API error: %s", errorMessage.c_str());
        }
    } else {
        errorMessage = curl_easy_strerror(res);
    }
    
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return {success, errorMessage, newServer, newKey};
}

std::string StreamlabsAPI::CategorySearch(const std::string& token, const std::string& category)
{
    if (category.empty() || category == "Other") return "";
    const std::atomic_bool cancelled{false};
    const auto result = SearchStreamlabsCategories(token, QString::fromStdString(category), cancelled);
    if (result.errorKey) return "";
    // Legacy configurations store only the full category name. Match that
    // exact name as the original app does; new selections use their saved ID.
    for (const auto &entry : result.categories)
        if (entry.name.compare(QString::fromStdString(category), Qt::CaseInsensitive) == 0)
            return entry.id.toStdString();
    return "";
}
