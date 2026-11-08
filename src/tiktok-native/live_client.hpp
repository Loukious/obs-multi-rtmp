// SPDX-License-Identifier: GPL-3.0-only
// Adapted from TikTok Live OBS; see data/licenses/tiktok-live-obs.txt.
#pragma once
#include <optional>
#include "tiktok_studio_account.hpp"
#include "tiktok_studio_game_tags.hpp"
#include "../tiktok-live-settings.h"
#include <QJsonObject>

struct TikTokStudioLive {
    TikTokStudioAccountCredentials account;
    QString room_id, stream_id, owner_user_id, server, key, dual_server, dual_key;
    bool created_by_us = false;
};

// Blocking operations: callers run these on a worker, never the encoder thread.
QString initialize_tiktok_account(TikTokStudioAccountCredentials &account);
TikTokStudioLive create_tiktok_live(TikTokStudioAccountCredentials account,
    const QString &title, const QString &topic_id, const QString &game_id, bool mature, QString *error,
    bool dual_layout = false, const TikTokLiveSettings& settings = {});
// Empty string means the account is eligible; otherwise a user-facing reason.
QString check_tiktok_dual_access(TikTokStudioAccountCredentials account);
struct TikTokDualAccess {
    std::optional<bool> available;
    QString reason;
};
TikTokDualAccess lookup_tiktok_dual_access(TikTokStudioAccountCredentials account);
QVector<TikTokStudioGameTag> fetch_tiktok_game_tags(const QByteArray &cookie_jar, QString *error);
QString ping_tiktok_live(TikTokStudioLive &live, int status);
QString end_tiktok_live(TikTokStudioLive &live);
QByteArray read_tiktok_cookie_file(const QString &path, QString *error);
QString lookup_tiktok_username(TikTokStudioAccountCredentials account, QString *error);
// Safety requests share the existing signer, cookies and regional host routing.
// POST sends JSON once only; a timeout never causes a second appeal submission.
QJsonObject tiktok_safety_request(TikTokStudioAccountCredentials& account, const QString& endpoint,
    const QJsonObject& fields, bool post, QString* error);
QJsonObject lookup_tiktok_account_identity(TikTokStudioAccountCredentials& account, QString* error);
