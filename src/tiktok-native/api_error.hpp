#pragma once
#include <QJsonObject>
#include <QString>

// TikTok and hosted signers often wrap a useful provider message in a generic
// outer error. Read only message fields, never serialize credential payloads.
inline QString tiktok_api_message(const QJsonObject& object)
{
    const auto read = [](const QJsonObject& value) {
        for (const char* key : {"prompts", "message", "status_msg", "description", "detail", "error"}) {
            const auto text = value.value(key).toString().trimmed();
            if (!text.isEmpty()) return text;
        }
        return QString();
    };
    for (const char* key : {"data", "error"}) {
        const auto nested = object.value(key).toObject();
        const auto detail = read(nested);
        if (!detail.isEmpty()) return detail;
    }
    return read(object);
}
