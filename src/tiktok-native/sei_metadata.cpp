// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "sei_metadata.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace {

constexpr std::array<qint64, 5> sei_offsets_ms = {0, 0, 16, 700, 1000};
constexpr qint64 sei_cycle_ms = 2000;
constexpr qint64 signature_select_interval_ms = 30000;
constexpr qint64 signature_tolerance_seconds = 1;
constexpr qint64 maximum_cadence_gap_ms = 10000;

QByteArray json_string(const QString &value)
{
	const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
	return array.size() >= 2 ? array.sliced(1, array.size() - 2) : QByteArrayLiteral("\"\"");
}

QByteArray sei_size_bytes(qsizetype size)
{
	QByteArray result;
	while (size >= 255) {
		result += static_cast<char>(0xff);
		size -= 255;
	}
	result += static_cast<char>(size);
	return result;
}

QByteArray escape_rbsp(const QByteArray &data)
{
	QByteArray result;
	result.reserve(data.size() + data.size() / 32);
	int zeros = 0;
	for (const char value : data) {
		const unsigned char byte = static_cast<unsigned char>(value);
		if (zeros >= 2 && byte <= 3) {
			result += static_cast<char>(3);
			zeros = 0;
		}
		result += value;
		zeros = byte == 0 ? zeros + 1 : 0;
	}
	return result;
}

bool is_annex_b(const QByteArray &packet)
{
	return packet.size() >= 3 && packet.at(0) == 0 && packet.at(1) == 0 &&
		(packet.at(2) == 1 || (packet.size() >= 4 && packet.at(2) == 0 && packet.at(3) == 1));
}

QByteArray annex_b_nal(SignedVideoCodec codec, const QByteArray &payload)
{
	const QByteArray nal = codec == SignedVideoCodec::Hevc
		? SignedSeiSession::build_hevc_sei(payload)
		: SignedSeiSession::build_h264_sei(payload);
	return QByteArray::fromHex("00000001") + nal;
}

} // namespace

bool SignedSeiConfig::complete() const
{
	return !uid.trimmed().isEmpty() && !device_id.trimmed().isEmpty() && !room_id.trimmed().isEmpty();
}

SignedSeiSession::SignedSeiSession(SignedSeiConfig config) : config_(std::move(config)) {}

void SignedSeiSession::merge_signatures(const QMap<qint64, FrameSignResult> &signatures)
{
	std::lock_guard lock(cache_mutex_);
	for (auto it = signatures.cbegin(); it != signatures.cend(); ++it)
		if (it.value().valid())
			signatures_.insert(it.key(), it.value());
	while (signatures_.size() > 1200)
		signatures_.erase(signatures_.begin());
}

qint64 SignedSeiSession::cache_last_timestamp() const
{
	std::lock_guard lock(cache_mutex_);
	return signatures_.isEmpty() ? 0 : signatures_.lastKey();
}

void SignedSeiSession::reset_stream()
{
	sign_result_.reset();
	last_signed_ms_ = 0;
	next_signature_select_stream_ms_ = -1;
	base_stream_ms_ = -1;
	last_stream_ms_ = -1;
	stream_started_ = false;
	next_slot_ = 0;
	sei_index_ = -1;
	startup_done_ = false;
}

QByteArray SignedSeiSession::build_h264_sei(const QByteArray &payload)
{
	const QByteArray rbsp = sei_size_bytes(100) + sei_size_bytes(payload.size()) + payload + static_cast<char>(0x80);
	return QByteArray(1, static_cast<char>(0x06)) + escape_rbsp(rbsp);
}

QByteArray SignedSeiSession::build_hevc_sei(const QByteArray &payload)
{
	const QByteArray rbsp = sei_size_bytes(100) + sei_size_bytes(payload.size()) + payload + static_cast<char>(0x80);
	return QByteArray::fromHex("4e01") + escape_rbsp(rbsp);
}

QByteArray SignedSeiSession::build_startup_payload(const FrameSignResult &sign_result,
	int width, int height, qint64 sei_index)
{
	return QByteArrayLiteral("JSON{\"live_sei_mute_mic\":{\"is_mute_mic\":0},\"push_video_height\":") +
		QByteArray::number(height) + QByteArrayLiteral(",\"push_video_width\":") + QByteArray::number(width) +
		QByteArrayLiteral(",\"sei_index\":") + QByteArray::number(sei_index) +
		QByteArrayLiteral(",\"signResult\":") + sign_result.compact_json() +
		QByteArrayLiteral(",\"ttls_live_scene\":\"live_studio\"}");
}

QByteArray SignedSeiSession::build_regular_payload(const FrameSignResult &sign_result,
	int width, int height, qint64 sei_index, qint64 timestamp_ms)
{
	return QByteArrayLiteral("JSON{\"live_sei_game_moment\":{\"timestamp\":") + QByteArray::number(timestamp_ms) +
		QByteArrayLiteral("},\"live_sei_mute_mic\":{\"is_mute_mic\":0},\"push_video_height\":") +
		QByteArray::number(height) + QByteArrayLiteral(",\"push_video_width\":") + QByteArray::number(width) +
		QByteArrayLiteral(",\"sei_index\":") + QByteArray::number(sei_index) +
		QByteArrayLiteral(",\"signResult\":") + sign_result.compact_json() +
		QByteArrayLiteral(",\"ts\":") + json_string(QString::number(timestamp_ms)) +
		QByteArrayLiteral(",\"ttls_live_scene\":\"live_studio\"}");
}

QByteArray SignedSeiSession::build_delay_payload(qint64 sei_index, qint64 timestamp_ms)
{
	return QByteArrayLiteral("JSON{\"sei_index\":") + QByteArray::number(sei_index) +
		QByteArrayLiteral(",\"video_e2e_delay\":{\"capture_window\":") + QByteArray::number(timestamp_ms) +
		QByteArrayLiteral(",\"encode\":") + QByteArray::number(timestamp_ms + 110) + QByteArrayLiteral("}}");
}

std::vector<SignedSeiSession::PayloadKind> SignedSeiSession::due_payloads(qint64 stream_timestamp_ms)
{
	constexpr std::array<PayloadKind, 5> kinds = {
		PayloadKind::SignedReuse,
		PayloadKind::Delay,
		PayloadKind::SignedCurrent,
		PayloadKind::SignedReuse,
		PayloadKind::SignedCurrent,
	};
	if (stream_started_ && (stream_timestamp_ms < last_stream_ms_ ||
		stream_timestamp_ms - last_stream_ms_ > maximum_cadence_gap_ms))
		reset_stream();
	last_stream_ms_ = stream_timestamp_ms;
	if (!stream_started_) {
		base_stream_ms_ = stream_timestamp_ms;
		stream_started_ = true;
	}
	const qint64 elapsed = std::max<qint64>(0, stream_timestamp_ms - base_stream_ms_);
	std::vector<PayloadKind> due;
	while (true) {
		const qint64 cycle = next_slot_ / static_cast<qint64>(sei_offsets_ms.size());
		const size_t slot = static_cast<size_t>(next_slot_ % static_cast<qint64>(sei_offsets_ms.size()));
		const qint64 target = cycle * sei_cycle_ms + sei_offsets_ms[slot];
		if (elapsed < target)
			break;
		PayloadKind kind = kinds[slot];
		if ((kind == PayloadKind::SignedReuse || kind == PayloadKind::SignedCurrent) && !startup_done_) {
			kind = PayloadKind::Startup;
			startup_done_ = true;
		}
		due.push_back(kind);
		++next_slot_;
	}
	return due;
}

std::optional<QPair<qint64, FrameSignResult>> SignedSeiSession::signature_near(qint64 timestamp_seconds) const
{
	std::lock_guard lock(cache_mutex_);
	if (signatures_.isEmpty())
		return std::nullopt;
	auto after = signatures_.lowerBound(timestamp_seconds);
	auto best = after;
	if (after == signatures_.end())
		best = std::prev(signatures_.end());
	else if (after != signatures_.begin()) {
		const auto before = std::prev(after);
		if (timestamp_seconds - before.key() <= after.key() - timestamp_seconds)
			best = before;
	}
	if (qAbs(best.key() - timestamp_seconds) > (config_.mobile ? 30 : signature_tolerance_seconds))
		return std::nullopt;
	return QPair<qint64, FrameSignResult>{best.key(), best.value()};
}

bool SignedSeiSession::ensure_signature(qint64 wall_clock_ms, QString *error)
{
	if (sign_result_)
		return true;
	const auto cached = signature_near(wall_clock_ms / 1000);
	if (!cached) {
		if (error)
			*error = QStringLiteral("No prefetched frame signature is available for the current timestamp.");
		return false;
	}
	last_signed_ms_ = wall_clock_ms;
	sign_result_ = cached->second;
	return true;
}

bool SignedSeiSession::refresh_signature_if_due(qint64 stream_timestamp_ms, qint64 wall_clock_ms, QString *error)
{
	if (next_signature_select_stream_ms_ < 0) {
		next_signature_select_stream_ms_ = stream_timestamp_ms + signature_select_interval_ms;
		return true;
	}
	if (stream_timestamp_ms < next_signature_select_stream_ms_)
		return true;
	while (stream_timestamp_ms >= next_signature_select_stream_ms_)
		next_signature_select_stream_ms_ += signature_select_interval_ms;
	const auto cached = signature_near(wall_clock_ms / 1000);
	if (!cached) {
		if (error)
			*error = QStringLiteral("The prefetched frame-signature cache expired.");
		return false;
	}
	last_signed_ms_ = wall_clock_ms;
	sign_result_ = cached->second;
	return true;
}

qint64 SignedSeiSession::next_index()
{
	return ++sei_index_;
}

bool SignedSeiSession::transform_packet(SignedVideoCodec codec, const QByteArray &packet, qint64 stream_timestamp_ms,
	int width, int height, qint64 wall_clock_ms, QByteArray *replacement, QString *error)
{
	if (replacement)
		replacement->clear();
	if (error)
		error->clear();
	if (!config_.complete()) {
		if (error)
			*error = QStringLiteral("TikTok UID, device ID, and room ID are required for frame signing.");
		return false;
	}
	if (!replacement || packet.isEmpty())
		return false;
	if (width <= 0 || height <= 0) {
		if (error)
			*error = QStringLiteral("OBS returned invalid encoded-video dimensions for frame signing.");
		return false;
	}
	if (!is_annex_b(packet)) {
		if (error)
			*error = QStringLiteral("The video encoder did not produce Annex-B H.264/HEVC packets.");
		return false;
	}

	const std::vector<PayloadKind> due = due_payloads(stream_timestamp_ms);
	if (due.empty())
		return false;
	if (!ensure_signature(wall_clock_ms, error))
		return false;

	QByteArray injected;
	for (const PayloadKind kind : due) {
		QByteArray payload;
		switch (kind) {
		case PayloadKind::Startup:
			payload = build_startup_payload(*sign_result_, width, height, next_index());
			break;
		case PayloadKind::Delay:
			payload = build_delay_payload(next_index(), wall_clock_ms);
			break;
		case PayloadKind::SignedReuse:
			payload = build_regular_payload(*sign_result_, width, height, next_index(),
				last_signed_ms_ > 0 ? last_signed_ms_ : wall_clock_ms);
			break;
		case PayloadKind::SignedCurrent:
			if (!refresh_signature_if_due(stream_timestamp_ms, wall_clock_ms, error))
				return false;
			payload = build_regular_payload(*sign_result_, width, height, next_index(), wall_clock_ms);
			break;
		}
		if (config_.mobile)
			payload.replace(",\"ttls_live_scene\":\"live_studio\"", "");
		injected += annex_b_nal(codec, payload);
	}
	replacement->reserve(injected.size() + packet.size());
	*replacement = std::move(injected);
	replacement->append(packet);
	return true;
}
