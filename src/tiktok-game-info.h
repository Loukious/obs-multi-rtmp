#pragma once
#include "tiktok-native/tiktok_studio_game_tags.hpp"
#include <atomic>
#include <functional>

struct TikTokGameResult {
    QVector<TikTokStudioGameTag> games;
    const char *errorKey = nullptr;
};
using TikTokGameLookup = std::function<TikTokGameResult(const QString &, const std::atomic_bool &)>;
TikTokGameResult LookupTikTokGames(const QString &cookieFile, const std::atomic_bool &cancelled);
