// Local import adapted from StreamLabsTikTokStreamKeyGenerator and
// Smile-4-Me/tiktok-live-obs/src/streamlabs_desktop.cpp.
#include "streamlabs-desktop.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

QString StreamlabsDesktopDirectory()
{
#ifdef Q_OS_WIN
    const auto root = qEnvironmentVariable("APPDATA");
#elif defined(Q_OS_MACOS)
    const auto root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
    const auto root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
    return root.isEmpty() ? QString() : QDir(root).filePath("slobs-client/Local Storage/leveldb");
}
QString FindStreamlabsDesktopToken(const QString& directory, const std::atomic_bool& cancelled)
{
    if (directory.isEmpty()) return {};
    const auto files = QDir(directory).entryInfoList({"*.log"}, QDir::Files | QDir::Readable, QDir::Time);
    static const QRegularExpression pattern(QStringLiteral("\"apiToken\"\\s*:\\s*\"([a-f0-9]+)\""), QRegularExpression::CaseInsensitiveOption);
    for (const auto& info : files) {
        if (cancelled) return {};
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        // Bound memory while handling large logs and UTF-16/NUL records.
        QByteArray tail;
        QString last;
        while (!file.atEnd() && !cancelled) {
            auto bytes = file.read(1024 * 1024);
            if (bytes.isEmpty()) break;
            bytes.replace(QByteArray(1, '\0'), QByteArray());
            tail += bytes;
            auto matches = pattern.globalMatch(QString::fromUtf8(tail));
            while (matches.hasNext()) last = matches.next().captured(1);
            tail = tail.right(16384);
        }
        if (cancelled) return {};
        if (!last.isEmpty()) return last;
    }
    return {};
}
