// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include <QByteArray>

// Public TikTok LIVE Studio desktop device-registration envelope ported from
// the tracked Loukious/TikTokStreamKeyGenerator reference implementation.
// Request and frame signatures are intentionally not implemented here.
QByteArray encrypt_tiktok_device_registration_payload(const QByteArray &json,
	qint64 unix_seconds);
