#pragma once

#include <string>
#include <functional>
#include <curl/curl.h>

class StreamlabsAPI {
public:
    static std::string ExtractStreamId(const std::string &key);
    static std::string CategorySearch(const std::string& token, const std::string& category);
    static std::tuple<bool, std::string, std::string, std::string> StartStream(const std::string& token, const std::string& title, const std::string& category, const int audienceType);
    static bool EndStream(const std::string &token, const std::string &streamID);

private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
};