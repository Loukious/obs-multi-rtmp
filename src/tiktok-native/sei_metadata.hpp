// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#pragma once

#include "frame_signing.hpp"

#include <QByteArray>
#include <QMap>
#include <QString>

#include <mutex>
#include <optional>
#include <vector>

enum class SignedVideoCodec { H264, Hevc };

struct SignedSeiConfig {
	QString aid = QStringLiteral("8311");
	QString uid;
	QString device_id;
	QString room_id;
	bool mobile = false;

	[[nodiscard]] bool complete() const;
};

class SignedSeiSession final {
public:
	explicit SignedSeiSession(SignedSeiConfig config);

	void merge_signatures(const QMap<qint64, FrameSignResult> &signatures);
	void reset_stream();
	[[nodiscard]] qint64 cache_last_timestamp() const;

	// Returns true only when replacement receives a newly allocated Annex-B
	// packet. False with an empty error means no metadata slot is due yet.
	bool transform_packet(SignedVideoCodec codec, const QByteArray &packet, qint64 stream_timestamp_ms,
		int width, int height, qint64 wall_clock_ms, QByteArray *replacement, QString *error = nullptr);

	[[nodiscard]] static QByteArray build_h264_sei(const QByteArray &payload);
	[[nodiscard]] static QByteArray build_hevc_sei(const QByteArray &payload);
	[[nodiscard]] static QByteArray build_startup_payload(const FrameSignResult &sign_result,
		int width, int height, qint64 sei_index);
	[[nodiscard]] static QByteArray build_regular_payload(const FrameSignResult &sign_result,
		int width, int height, qint64 sei_index, qint64 timestamp_ms);
	[[nodiscard]] static QByteArray build_delay_payload(qint64 sei_index, qint64 timestamp_ms);

private:
	enum class PayloadKind { Startup, SignedReuse, SignedCurrent, Delay };

	std::vector<PayloadKind> due_payloads(qint64 stream_timestamp_ms);
	std::optional<QPair<qint64, FrameSignResult>> signature_near(qint64 timestamp_seconds) const;
	bool ensure_signature(qint64 wall_clock_ms, QString *error);
	bool refresh_signature_if_due(qint64 stream_timestamp_ms, qint64 wall_clock_ms, QString *error);
	[[nodiscard]] qint64 next_index();

	SignedSeiConfig config_;
	mutable std::mutex cache_mutex_;
	QMap<qint64, FrameSignResult> signatures_;
	std::optional<FrameSignResult> sign_result_;
	qint64 last_signed_ms_ = 0;
	qint64 next_signature_select_stream_ms_ = -1;
	qint64 base_stream_ms_ = -1;
	qint64 last_stream_ms_ = -1;
	bool stream_started_ = false;
	qint64 next_slot_ = 0;
	qint64 sei_index_ = -1;
	bool startup_done_ = false;
};
