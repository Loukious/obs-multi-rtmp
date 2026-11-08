// SPDX-License-Identifier: GPL-3.0-only
// Adapted from TikTok Live OBS; see data/licenses/tiktok-live-obs.txt.
#pragma once
#include "tiktok_studio_account.hpp"

enum class TikTokQrState { Waiting, Scanned, Confirmed, Expired, Refused, Error };
struct TikTokQrLogin {
    TikTokStudioAccountCredentials account;
    QByteArray png;
    QString token, host, client_secret, error;
    TikTokQrState state = TikTokQrState::Waiting;
    bool retryable = false;
};
// Blocking; run on a worker with TikTokRequestCancellation.
TikTokQrLogin begin_tiktok_qr_login(TikTokStudioAccountCredentials account);
TikTokQrLogin poll_tiktok_qr_login(TikTokQrLogin login);
QString save_tiktok_login_cookies(const QByteArray& jar, const QString& directory, QString* error, const QString& username = {});
