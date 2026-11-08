// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "frame_signing.hpp"
#include "sei_metadata.hpp"

#include <QByteArray>
#include <QMap>

#include <iostream>
#include <optional>

namespace {

bool expect(bool condition, const char *message)
{
	if (!condition)
		std::cerr << message << '\n';
	return condition;
}

FrameSignResult sample_signature()
{
	return {QStringLiteral("2"), QStringLiteral("1877999593"), QStringLiteral("sample-sign-info"),
		QStringLiteral("sample-sign-value"), QStringLiteral("1.0")};
}

QMap<qint64, FrameSignResult> signature_window(qint64 first, qint64 last)
{
	QMap<qint64, FrameSignResult> result;
	for (qint64 timestamp = first; timestamp <= last; ++timestamp)
		result.insert(timestamp, sample_signature());
	return result;
}

std::optional<QByteArray> decode_sei_payload(const QByteArray &nal, qsizetype header_size)
{
	if (nal.size() <= header_size)
		return std::nullopt;
	QByteArray rbsp;
	int zeros = 0;
	for (qsizetype index = header_size; index < nal.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(nal.at(index));
		if (zeros >= 2 && byte == 3) {
			zeros = 0;
			continue;
		}
		rbsp += nal.at(index);
		zeros = byte == 0 ? zeros + 1 : 0;
	}
	qsizetype cursor = 0;
	auto read_ff_value = [&rbsp, &cursor]() -> std::optional<qsizetype> {
		qsizetype value = 0;
		while (cursor < rbsp.size() && static_cast<unsigned char>(rbsp.at(cursor)) == 0xff) {
			value += 255;
			++cursor;
		}
		if (cursor >= rbsp.size())
			return std::nullopt;
		value += static_cast<unsigned char>(rbsp.at(cursor++));
		return value;
	};
	const auto payload_type = read_ff_value();
	const auto payload_size = read_ff_value();
	if (!payload_type || *payload_type != 100 || !payload_size ||
		*payload_size < 0 || cursor + *payload_size > rbsp.size())
		return std::nullopt;
	return rbsp.sliced(cursor, *payload_size);
}

} // namespace

int main()
{
	bool passed = true;
	const FrameSignInput input{QStringLiteral("8311"), QStringLiteral("1001"), QStringLiteral("2002"),
		QStringLiteral("3003"), QStringLiteral("2"), 1700000000};
	const QByteArray response = R"({"success":true,"signatures":[{"timestamp":1700000000,"signResult":{"frametype":"2","lid":"1877999593","signinfo":"abc","signvalue":"def","signversion":"1.0"}},{"timestamp":1700000001,"frametype":"2","lid":"1877999593","signinfo":"ghi","signvalue":"jkl","signversion":"1.0"}]})";
	const FrameSignBatch parsed = FrameSignClient::parse_batch_response(response, 200, input);
	passed &= expect(parsed.valid(), "valid RapidAPI batch fixture was rejected");
	passed &= expect(parsed.signatures.size() == 2, "RapidAPI batch fixture did not produce two signatures");
	passed &= expect(parsed.last_timestamp() == 1700000001, "RapidAPI batch timestamp normalization failed");
	const FrameSignBatch array_parsed = FrameSignClient::parse_batch_response(
		QByteArrayLiteral("[{\"timestamp\":1700000002,\"signinfo\":\"mno\",\"signvalue\":\"pqr\"}]"),
		200, input);
	passed &= expect(array_parsed.valid() && array_parsed.first_timestamp() == 1700000002,
		"top-level RapidAPI batch array was rejected");
	HostedSigningServiceConfig api;
	api.api_key = QStringLiteral("fixture-key");
	passed &= expect(api.valid(), "default RapidAPI endpoint was rejected");
	api.base_url = QUrl(QStringLiteral("https://rapidapi.com.evil.example/"));
	passed &= expect(!api.valid(), "a lookalike RapidAPI hostname was accepted");
	api.base_url = QUrl(QStringLiteral("https://tiktok-live-studio-api-signer1.p.rapidapi.com/"));
	api.api_key = QStringLiteral("fixture-key\r\nInjected: value");
	passed &= expect(!api.valid(), "a RapidAPI key containing a header break was accepted");

	const FrameSignBatch rejected = FrameSignClient::parse_batch_response(
		QByteArrayLiteral("{\"success\":false,\"error\":\"quota exceeded\"}"), 429, input);
	passed &= expect(!rejected.valid() && rejected.error.contains(QStringLiteral("quota exceeded")),
		"RapidAPI error response was not rejected safely");
	const FrameSignBatch blank = FrameSignClient::parse_batch_response(
		QByteArrayLiteral("{\"success\":true,\"signatures\":[{\"timestamp\":1700000000,\"signinfo\":\"\",\"signvalue\":\"\"}]}"),
		200, input);
	passed &= expect(!blank.valid(), "blank frame signatures were accepted");

	const SignedSeiConfig config{QStringLiteral("8311"), QStringLiteral("1001"),
		QStringLiteral("2002"), QStringLiteral("3003")};
	passed &= expect(sample_signature().compact_json() == QByteArrayLiteral(
		"{\"frametype\":\"2\",\"lid\":\"1877999593\",\"signinfo\":\"sample-sign-info\","
		"\"signvalue\":\"sample-sign-value\",\"signversion\":\"1.0\"}"),
		"signResult JSON did not retain the required compact field shape");
	const QByteArray escaped_payload = QByteArrayLiteral("JSON-roundtrip-") +
		QByteArray::fromHex("000001000002000003");
	passed &= expect(decode_sei_payload(SignedSeiSession::build_h264_sei(escaped_payload), 1) == escaped_payload,
		"H.264 payload-type-100 RBSP framing did not round-trip");
	passed &= expect(decode_sei_payload(SignedSeiSession::build_hevc_sei(escaped_payload), 2) == escaped_payload,
		"HEVC payload-type-100 RBSP framing did not round-trip");
	SignedSeiSession h264(config);
	h264.merge_signatures(signature_window(1700000000, 1700000300));
	const QByteArray h264_packet = QByteArray::fromHex("0000000165888421");
	QByteArray transformed;
	QString error;
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 0, 1920, 1080,
		1700000000000LL, &transformed, &error), "startup H.264 packet was not transformed");
	passed &= expect(error.isEmpty(), "startup H.264 transform returned an error");
	passed &= expect(transformed.startsWith(QByteArray::fromHex("0000000106")),
		"H.264 SEI NAL was not prepended in Annex-B form");
	passed &= expect(transformed.endsWith(h264_packet), "original H.264 access unit was not preserved");
	passed &= expect(transformed.count("JSON") == 2, "startup cadence should inject signed and delay metadata together");

	QByteArray not_due;
	passed &= expect(!h264.transform_packet(SignedVideoCodec::H264, h264_packet, 1, 1920, 1080,
		1700000000001LL, &not_due, &error) && error.isEmpty(), "non-due H.264 packet was modified");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 16, 1920, 1080,
		1700000000016LL, &transformed, &error), "16 ms signed cadence slot was missed");
	passed &= expect(transformed.count("JSON") == 1, "16 ms cadence slot should inject one signed payload");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 700, 1920, 1080,
		1700000000700LL, &transformed, &error) && transformed.count("JSON") == 1,
		"700 ms reused-signature cadence slot was missed");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 1000, 1920, 1080,
		1700000001000LL, &transformed, &error) && transformed.count("JSON") == 1,
		"1000 ms signed cadence slot was missed");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 2000, 1920, 1080,
		1700000002000LL, &transformed, &error) && transformed.count("JSON") == 2,
		"second cadence cycle did not start with signed and delay metadata");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 0, 1920, 1080,
		1700000003000LL, &transformed, &error) && transformed.count("JSON") == 2,
		"a restarted output did not reset to the startup signing cadence");
	passed &= expect(h264.transform_packet(SignedVideoCodec::H264, h264_packet, 60000, 1920, 1080,
		1700000060000LL, &transformed, &error) && transformed.count("JSON") == 2,
		"a large encoder timestamp gap attempted unbounded cadence catch-up");

	const QByteArray avcc_packet = QByteArray::fromHex("00000004658800000001");
	SignedSeiSession avcc(config);
	avcc.merge_signatures(signature_window(1700000000, 1700000300));
	passed &= expect(!avcc.transform_packet(SignedVideoCodec::H264, avcc_packet, 0, 1920, 1080,
		1700000000000LL, &transformed, &error) && !error.isEmpty(),
		"an AVCC packet with an embedded start-code pattern was mistaken for Annex-B");

	SignedSeiSession hevc(config);
	hevc.merge_signatures(signature_window(1700000000, 1700000300));
	const QByteArray hevc_packet = QByteArray::fromHex("0000000126019988");
	passed &= expect(hevc.transform_packet(SignedVideoCodec::Hevc, hevc_packet, 0, 1080, 1920,
		1700000000000LL, &transformed, &error), "startup HEVC packet was not transformed");
	passed &= expect(transformed.startsWith(QByteArray::fromHex("000000014e01")),
		"HEVC prefix SEI NAL was not prepended in Annex-B form");
	passed &= expect(transformed.endsWith(hevc_packet), "original HEVC access unit was not preserved");

	SignedSeiSession expired(config);
	expired.merge_signatures(signature_window(1700000000, 1700000000));
	passed &= expect(!expired.transform_packet(SignedVideoCodec::H264, h264_packet, 0, 1920, 1080,
		1700000005000LL, &transformed, &error) && !error.isEmpty(),
		"expired signature cache did not fail closed");

	return passed ? 0 : 1;
}
