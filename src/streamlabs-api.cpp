#include "streamlabs-api.h"
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
            if (responseObj.contains("data")) {
                QJsonObject dataObj = responseObj["data"].toObject();
                if (dataObj.contains("message")) {
                    blog(LOG_INFO, "Streamlabs API detailed error: %s", 
                        dataObj["message"].toString().toStdString().c_str());
                }
            }
            
            if (responseObj.contains("message")) {
                errorMessage = responseObj["message"].toString().toStdString();
            } else {
                errorMessage = "Unknown error occurred";
            }
        }
    } else {
        errorMessage = curl_easy_strerror(res);
    }
    
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return {success, errorMessage, newServer, newKey};
}

static int levenshteinDistance(const std::string& s1, const std::string& s2) {
    const int m = static_cast<int>(s1.size());
    const int n = static_cast<int>(s2.size());
    if (m == 0) return n;
    if (n == 0) return m;

    std::vector<int> costs(n + 1);
    for (int i = 0; i <= n; ++i) {
        costs[i] = i;
    }

    for (int i = 0; i < m; ++i) {
        costs[0] = i + 1;
        int corner = i;
        for (int j = 0; j < n; ++j) {
            int upper = costs[j + 1];
            costs[j + 1] = std::min({
                costs[j] + 1,
                upper + 1,
                corner + (s1[static_cast<size_t>(i)] == s2[static_cast<size_t>(j)] ? 0 : 1)
            });
            corner = upper;
        }
    }
    return costs[n];
}

std::string StreamlabsAPI::CategorySearch(const std::string& token, const std::string& category) {
    if (category.empty()) {
        blog(LOG_WARNING, TAG "CategorySearch failed: Empty category input");
        return "";
    }

    std::string truncated_category = category.substr(0, 25);
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        blog(LOG_WARNING, TAG "CategorySearch failed: Could not initialize cURL");
        return "";
    }

    char* escaped = curl_easy_escape(curl, truncated_category.c_str(), static_cast<int>(truncated_category.length()));
    if (!escaped) {
        blog(LOG_WARNING, TAG "CategorySearch failed: URL escaping failed for category '%s'", truncated_category.c_str());
        curl_easy_cleanup(curl);
        return "";
    }
    
    std::string url = "https://streamlabs.com/api/v5/slobs/tiktok/info?category=" + std::string(escaped);
    curl_free(escaped);
    
    std::string response_data;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    
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

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        blog(LOG_WARNING, TAG "CategorySearch failed: cURL request failed with error: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_cleanup(curl);

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(response_data.c_str(), static_cast<int>(response_data.size())), &parseError);
    if (doc.isNull() || parseError.error != QJsonParseError::NoError) {
        blog(LOG_WARNING, TAG "CategorySearch failed: JSON parse error at offset %d: %s", 
             parseError.offset, parseError.errorString().toUtf8().constData());
        return "";
    }

    QJsonObject root = doc.object();
    if (!root.contains("categories")) {
        blog(LOG_WARNING, TAG "CategorySearch failed: Response missing 'categories' field");
        return "";
    }

    QJsonValue categoriesValue = root.value("categories");
    if (!categoriesValue.isArray()) {
        blog(LOG_WARNING, TAG "CategorySearch failed: 'categories' field is not an array");
        return "";
    }

    QJsonArray categories = categoriesValue.toArray();
    if (categories.isEmpty()) {
        blog(LOG_WARNING, TAG "CategorySearch failed: No categories found for '%s'", truncated_category.c_str());
        return "";
    }

    std::string originalLower = category;
    std::transform(originalLower.begin(), originalLower.end(), originalLower.begin(), ::tolower);

    QJsonObject bestMatch;
    bool exactMatchFound = false;

    for (const QJsonValue& val : categories) {
        if (!val.isObject()) {
            continue;
        }
        QJsonObject obj = val.toObject();
        if (!obj.contains("full_name")) {
            continue;
        }
        QJsonValue nameVal = obj.value("full_name");
        if (!nameVal.isString()) {
            continue;
        }
        std::string fullName = nameVal.toString().toStdString();
        std::string fullNameLower = fullName;
        std::transform(fullNameLower.begin(), fullNameLower.end(), fullNameLower.begin(), ::tolower);

        if (fullNameLower == originalLower) {
            bestMatch = obj;
            exactMatchFound = true;
            break;
        }
    }

    if (!exactMatchFound) {
        int minDistance = INT_MAX;
        for (const QJsonValue& val : categories) {
            if (!val.isObject()) {
                continue;
            }
            QJsonObject obj = val.toObject();
            if (!obj.contains("full_name")) {
                continue;
            }
            QJsonValue nameVal = obj.value("full_name");
            if (!nameVal.isString()) {
                continue;
            }
            std::string fullName = nameVal.toString().toStdString();
            std::string fullNameLower = fullName;
            std::transform(fullNameLower.begin(), fullNameLower.end(), fullNameLower.begin(), ::tolower);

            int distance = levenshteinDistance(originalLower, fullNameLower);
            if (distance < minDistance) {
                minDistance = distance;
                bestMatch = obj;
            } else if (distance == minDistance) {
                std::string currentBestName = bestMatch["full_name"].toString().toStdString();
                std::string candidateName = obj["full_name"].toString().toStdString();
                if (candidateName.length() > currentBestName.length()) {
                    bestMatch = obj;
                }
            }
        }
    }

    if (bestMatch.isEmpty()) {
        blog(LOG_WARNING, TAG "CategorySearch failed: No valid category match found for '%s'", category.c_str());
        return "";
    }

    if (!bestMatch.contains("game_mask_id")) {
        blog(LOG_WARNING, TAG "CategorySearch failed: Best match category missing 'game_mask_id'");
        return "";
    }

    QJsonValue gameMaskIdValue = bestMatch.value("game_mask_id");
    if (!gameMaskIdValue.isString()) {
        blog(LOG_WARNING, TAG "CategorySearch failed: 'game_mask_id' is not a string in best match");
        return "";
    }

    return gameMaskIdValue.toString().toStdString();
}
