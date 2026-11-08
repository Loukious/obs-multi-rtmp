#include "tiktok-game-info.h"
#include "tiktok-native/live_client.hpp"
#include "tiktok-native/request_cancellation.hpp"

TikTokGameResult LookupTikTokGames(const QString &cookieFile, const std::atomic_bool &cancelled)
{
    if (cancelled) return {};
    TikTokRequestCancellation cancellation(&cancelled);
    QString error;
    const auto jar = read_tiktok_cookie_file(cookieFile, &error);
    if (!error.isEmpty()) return {{}, "Account.InvalidCookies"};
    const auto games = fetch_tiktok_game_tags(jar, &error);
    return error.isEmpty() ? TikTokGameResult{games} : TikTokGameResult{{}, "Topic.GamesUnavailable"};
}
