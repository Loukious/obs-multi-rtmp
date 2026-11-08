#pragma once
#include <curl/curl.h>

// Recovery classification only; callers decide whether an operation is safe
// to retry. In particular this must not enable automatic LIVE/appeal retries.
inline bool tiktok_transient_transport_error(CURLcode code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return false;
    }
}

inline bool tiktok_transient_http_status(long status)
{
    return status == 408 || (status >= 500 && status < 600);
}
