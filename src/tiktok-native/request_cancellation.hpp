#pragma once
#include <atomic>
#include <curl/curl.h>

inline thread_local const std::atomic_bool *tiktok_request_cancelled = nullptr;
struct TikTokRequestCancellation {
    const std::atomic_bool *previous;
    explicit TikTokRequestCancellation(const std::atomic_bool *value) : previous(tiktok_request_cancelled)
    { tiktok_request_cancelled = value; }
    ~TikTokRequestCancellation() { tiktok_request_cancelled = previous; }
};
inline bool tiktok_cancel_requested() { return tiktok_request_cancelled && tiktok_request_cancelled->load(); }
inline int tiktok_cancel_transfer(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{ return tiktok_cancel_requested() ? 1 : 0; }
inline void set_tiktok_request_cancellation(CURL *handle)
{
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, tiktok_cancel_transfer);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, nullptr);
}
