// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 TikTok Live OBS Contributors

#include "tiktok_studio_qr.hpp"

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QUrl>
#include <QUrlQuery>

#include <qrcodegen.hpp>

#include <exception>

QString bind_tiktok_qr_client_secret(const QString &encoded_url,
	const QString &client_secret)
{
	if (encoded_url.isEmpty() || client_secret.isEmpty())
		return encoded_url;
	const QString decoded = QUrl::fromPercentEncoding(encoded_url.toUtf8());
	const int separator = decoded.indexOf(QLatin1Char('?'));
	if (separator < 0)
		return encoded_url;
	QUrl url(decoded);
	QUrlQuery query(url);
	QString next_url = query.queryItemValue(QStringLiteral("next_url"), QUrl::FullyDecoded);
	if (next_url.isEmpty())
		return encoded_url;
	QUrl next(next_url);
	QUrlQuery next_query(next);
	next_query.addQueryItem(QStringLiteral("client_secret"), client_secret);
	next.setQuery(next_query);
	query.removeAllQueryItems(QStringLiteral("next_url"));
	query.addQueryItem(QStringLiteral("next_url"), next.toString(QUrl::FullyEncoded));
	url.setQuery(query);
	return url.toString(QUrl::FullyEncoded);
}

QByteArray render_tiktok_qr_png(const QString &value)
{
	if (value.isEmpty())
		return {};
	try {
		const QByteArray encoded = value.toUtf8();
		const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(encoded.constData(),
			qrcodegen::QrCode::Ecc::MEDIUM);
		constexpr int border = 4;
		const int modules = qr.getSize() + border * 2;
		const int scale = qMax(1, 256 / modules);
		QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
		image.fill(Qt::white);
		QPainter painter(&image);
		painter.setPen(Qt::NoPen);
		painter.setBrush(Qt::black);
		for (int y = 0; y < qr.getSize(); ++y) {
			for (int x = 0; x < qr.getSize(); ++x) {
				if (qr.getModule(x, y))
					painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
			}
		}
		painter.end();
		QByteArray png;
		QBuffer buffer(&png);
		if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
			return {};
		return png;
	} catch (const std::exception &) {
		return {};
	}
}
