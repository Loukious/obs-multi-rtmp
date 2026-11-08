// Load only in an isolated portable OBS installation, never in a user's OBS.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>

OBS_DECLARE_MODULE()

static void OnLoaded(obs_frontend_event event, void*)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;
    QTimer::singleShot(1500, [] {
        auto module = obs_get_module("obs-multi-rtmp");
        const QString binary = module ? QFileInfo(obs_get_module_binary_path(module)).canonicalFilePath() : QString();
        const QString data = module ? QFileInfo(obs_get_module_data_path(module)).canonicalFilePath() : QString();
        const QString expected = QFileInfo(qEnvironmentVariable("MULTIRTMP_SMOKE_BINARY")).canonicalFilePath();
        const bool modern = !expected.isEmpty() && binary == expected
            && data == QFileInfo(binary).absolutePath() + "/data";
        const char* translated = nullptr;
        const bool localized = module && obs_module_get_locale_string(module, "Btn.Start", &translated);
        QFile report(qEnvironmentVariable("MULTIRTMP_SMOKE_REPORT"));
        if (report.open(QIODevice::WriteOnly))
            report.write(QJsonDocument(QJsonObject{{"passed", modern && localized}, {"binary", binary},
                {"locale_loaded", localized},
                {"data", data}, {"obs_version", obs_get_version_string()},
                {"no_broadcast_started", !obs_frontend_streaming_active()}}).toJson());
        QTimer::singleShot(300, [] { static_cast<QWidget*>(obs_frontend_get_main_window())->close(); });
    });
}

bool obs_module_load()
{
    if (qEnvironmentVariableIsEmpty("MULTIRTMP_SMOKE_REPORT"))
        return false;
    obs_frontend_add_event_callback(OnLoaded, nullptr);
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_event_callback(OnLoaded, nullptr);
}
