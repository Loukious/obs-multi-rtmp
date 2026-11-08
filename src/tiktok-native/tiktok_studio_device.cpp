// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_studio_device.hpp"

#include <QtEndian>

#include <array>
#include <cstdint>

namespace {

constexpr std::array<uint8_t, 256> sbox0{
	99, 124, 119, 123, 242, 107, 111, 197, 48, 1, 103, 43, 254, 215, 171, 118,
	202, 130, 201, 125, 250, 89, 71, 240, 173, 212, 162, 175, 156, 164, 114, 192,
	183, 253, 147, 38, 54, 63, 247, 204, 52, 165, 229, 241, 113, 216, 49, 21,
	4, 199, 35, 195, 24, 150, 5, 154, 7, 18, 128, 226, 235, 39, 178, 117,
	9, 131, 44, 26, 27, 110, 90, 160, 82, 59, 214, 179, 41, 227, 47, 132,
	83, 209, 0, 237, 32, 252, 177, 91, 106, 203, 190, 57, 74, 76, 88, 207,
	208, 239, 170, 251, 67, 77, 51, 133, 69, 249, 2, 127, 80, 60, 159, 168,
	81, 163, 64, 143, 146, 157, 56, 245, 188, 182, 218, 33, 16, 255, 243, 210,
	205, 12, 19, 236, 95, 151, 68, 23, 196, 167, 126, 61, 100, 93, 25, 115,
	96, 129, 79, 220, 34, 42, 144, 136, 70, 238, 184, 20, 222, 94, 11, 219,
	224, 50, 58, 10, 73, 6, 36, 92, 194, 211, 172, 98, 145, 149, 228, 121,
	231, 200, 55, 109, 141, 213, 78, 169, 108, 86, 244, 234, 101, 122, 174, 8,
	186, 120, 37, 46, 28, 166, 180, 198, 232, 221, 116, 31, 75, 189, 139, 138,
	112, 62, 181, 102, 72, 3, 246, 14, 97, 53, 87, 185, 134, 193, 29, 158,
	225, 248, 152, 17, 105, 217, 142, 148, 155, 30, 135, 233, 206, 85, 40, 223,
	140, 161, 137, 13, 191, 230, 66, 104, 65, 153, 45, 15, 176, 84, 187, 22,
};

constexpr std::array<uint8_t, 256> sbox1{
	82, 9, 106, 213, 48, 54, 165, 56, 191, 64, 163, 158, 129, 243, 215, 251,
	124, 227, 57, 130, 155, 47, 255, 135, 52, 142, 67, 68, 196, 222, 233, 203,
	84, 123, 148, 50, 166, 194, 35, 61, 238, 76, 149, 11, 66, 250, 195, 78,
	8, 46, 161, 102, 40, 217, 36, 178, 118, 91, 162, 73, 109, 139, 209, 37,
	114, 248, 246, 100, 134, 104, 152, 22, 212, 164, 92, 204, 93, 101, 182, 146,
	108, 112, 72, 80, 253, 237, 185, 218, 94, 21, 70, 87, 167, 141, 157, 132,
	144, 216, 171, 0, 140, 188, 211, 10, 247, 228, 88, 5, 184, 179, 69, 6,
	208, 44, 30, 143, 202, 63, 15, 2, 193, 175, 189, 3, 1, 19, 138, 107,
	58, 145, 17, 65, 79, 103, 220, 234, 151, 242, 207, 206, 240, 180, 230, 115,
	150, 172, 116, 34, 231, 173, 53, 133, 226, 249, 55, 232, 28, 117, 223, 110,
	71, 241, 26, 113, 29, 41, 197, 137, 111, 183, 98, 14, 170, 24, 190, 27,
	252, 86, 62, 75, 198, 210, 121, 32, 154, 219, 192, 254, 120, 205, 90, 244,
	31, 221, 168, 51, 136, 7, 199, 49, 177, 18, 16, 89, 39, 128, 236, 95,
	96, 81, 127, 169, 25, 181, 74, 13, 45, 229, 122, 159, 147, 201, 156, 239,
	160, 224, 59, 77, 174, 42, 245, 176, 200, 235, 187, 60, 131, 83, 153, 97,
	23, 43, 4, 126, 186, 119, 214, 38, 225, 105, 20, 99, 85, 33, 12, 125,
};

constexpr auto registration_key = "I+D&*76:j27kVH<us9&d";

uint32_t rotate_left(uint32_t value, unsigned amount)
{
	return (value << amount) | (value >> (32 - amount));
}

uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
		(uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

void append_be32(QByteArray &out, uint32_t value)
{
	out.append(char((value >> 24) & 0xff));
	out.append(char((value >> 16) & 0xff));
	out.append(char((value >> 8) & 0xff));
	out.append(char(value & 0xff));
}

void append_le32(QByteArray &out, uint32_t value)
{
	out.append(char(value & 0xff));
	out.append(char((value >> 8) & 0xff));
	out.append(char((value >> 16) & 0xff));
	out.append(char((value >> 24) & 0xff));
}

uint32_t crc32(const QByteArray &data)
{
	uint32_t crc = 0xffffffffu;
	for (const char raw : data) {
		crc ^= static_cast<uint8_t>(raw);
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

QByteArray gzip_payload(const QByteArray &payload, qint64 unix_seconds)
{
	// qCompress adds a four-byte size prefix and a zlib wrapper. Reuse its
	// deflate stream inside the gzip envelope expected by desktop registration.
	const QByteArray compressed = qCompress(payload, 6);
	if (compressed.size() < 10)
		return {};
	const QByteArray zlib_stream = compressed.mid(4);
	if (zlib_stream.size() < 6)
		return {};
	const QByteArray deflate = zlib_stream.mid(2, zlib_stream.size() - 6);
	QByteArray out;
	out.reserve(10 + deflate.size() + 8);
	out.append("\x1f\x8b\x08\x00", 4);
	append_le32(out, static_cast<uint32_t>(unix_seconds));
	out.append("\x00\x03", 2);
	out += deflate;
	append_le32(out, crc32(payload));
	append_le32(out, static_cast<uint32_t>(payload.size()));
	return out;
}

std::array<uint32_t, 4> derived_key()
{
	std::array<uint8_t, 16> bytes{};
	const QByteArray key(registration_key);
	const int copy = qMin(16, key.size());
	for (int index = 0; index < copy; ++index)
		bytes[index] = static_cast<uint8_t>(key.at(index));
	for (int index = copy; index < 16; ++index)
		bytes[index] = sbox1[bytes[index - copy]];
	for (uint8_t &byte : bytes)
		byte = sbox0[byte];
	return {read_be32(bytes.data()), read_be32(bytes.data() + 4),
		read_be32(bytes.data() + 8), read_be32(bytes.data() + 12)};
}

} // namespace

QByteArray encrypt_tiktok_device_registration_payload(const QByteArray &json,
	qint64 unix_seconds)
{
	const QByteArray compressed = gzip_payload(json, unix_seconds);
	if (compressed.isEmpty())
		return {};
	QByteArray padded = compressed;
	const int padding = 16 - (padded.size() % 16);
	const uint8_t header_padding = padding == 16 ? 0 : static_cast<uint8_t>(padding);
	if (padding != 16)
		padded.append(QByteArray(padding, char(padding)));
	for (char &byte : padded)
		byte = char(sbox0[static_cast<uint8_t>(byte)]);

	const auto key = derived_key();
	QByteArray encrypted;
	encrypted.reserve(6 + padded.size());
	encrypted.append(char(29795 >> 8));
	encrypted.append(char(29795 & 0xff));
	encrypted.append(char(3));
	encrypted.append(char(header_padding));
	encrypted.append(char(0));
	encrypted.append(char(3));
	for (int offset = 0; offset < padded.size(); offset += 16) {
		const auto *block = reinterpret_cast<const uint8_t *>(padded.constData() + offset);
		const uint32_t first = read_be32(block) ^ key[0];
		const uint32_t second = rotate_left(read_be32(block + 4), 8) ^ key[1];
		const uint32_t third = rotate_left(read_be32(block + 8), 16) ^ key[2];
		const uint32_t fourth = rotate_left(read_be32(block + 12), 24) ^ key[3];
		append_be32(encrypted, first);
		append_be32(encrypted, second);
		append_be32(encrypted, third);
		append_be32(encrypted, fourth);
	}
	return encrypted;
}
