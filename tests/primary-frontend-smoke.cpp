#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QTimer>
#include <Windows.h>

OBS_DECLARE_MODULE()

namespace {
QPointer<QTimer> driver;
QPointer<QDialog> settings;
int stage = 0, ticks = 0;
void Done(const QString& error = {}) {
    driver->stop();
    const auto path = qEnvironmentVariable("TIKTOK_PRIMARY_SMOKE_REPORT");
    QFile report(path);
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject{
        {"passed", error.isEmpty()}, {"stage", stage}, {"error", error},
        {"obs_version", obs_get_version_string()}, {"no_broadcast_started", !obs_frontend_streaming_active()}
    }).toJson());
    if (settings) settings->reject();
    QTimer::singleShot(300, [] { static_cast<QWidget*>(obs_frontend_get_main_window())->close(); });
}
bool Require(bool success, const char* message) { if (!success) Done(message); return success; }
void Tick() {
    if (++ticks > 180) { Done("Native frontend smoke test timed out"); return; }
    auto main = static_cast<QWidget*>(obs_frontend_get_main_window());
    if (stage == 0) {
        stage = 1;
        QMetaObject::invokeMethod(main, "on_action_Settings_triggered", Qt::QueuedConnection);
        return;
    }
    if (!settings) {
        for (auto widget : QApplication::topLevelWidgets())
            if (QByteArray(widget->metaObject()->className()) == "OBSBasicSettings" && widget->isVisible()) settings = qobject_cast<QDialog*>(widget);
        if (!settings) return;
    }
    auto provider = settings->findChild<QComboBox*>("primaryStreamingProvider");
    if (!Require(provider && settings->property("tiktokPrimaryAttached").toBool(), "TikTok adapter missing from actual OBS Settings")) return;
    auto buttons = settings->findChild<QDialogButtonBox*>("buttonBox");
    if (stage == 1) {
        auto categories = settings->findChild<QListWidget*>("listWidget");
        for (int i = 0; i < categories->count(); ++i) if (categories->item(i)->text() == "Stream") categories->setCurrentRow(i);
        settings->findChild<QComboBox*>("service")->setCurrentIndex(0);
        settings->findChild<QLineEdit*>("customServer")->setText("rtmp://127.0.0.1/fixture");
        settings->findChild<QLineEdit*>("key")->setText("fixture");
        buttons->button(QDialogButtonBox::Apply)->click();
        OBSService service = obs_frontend_get_streaming_service();
        OBSDataAutoRelease data = obs_service_get_settings(service);
        if (!Require(QString(obs_service_get_id(service)) == "rtmp_custom" && QString(obs_data_get_string(data, "server")) == "rtmp://127.0.0.1/fixture", "Native Apply no longer saves normal OBS services")) return;
        QApplication::processEvents();
        settings->grab().save(qEnvironmentVariable("TIKTOK_PRIMARY_SMOKE_REPORT") + ".default.png");
        provider->setCurrentIndex(1); stage = 2; return;
    }
    if (stage == 2) {
        // File dialogs update the input via setText, not textEdited.
        auto method = settings->findChild<QComboBox*>("connectionMethod");
        method->setCurrentIndex(2);
        auto quota = settings->findChild<QProgressBar*>("rapidApiQuotaBar");
        if (!Require(quota && quota->isVisible() && quota->text() == "Unknown", "RapidAPI quota meter missing from main OBS settings")) return;
        auto safety = settings->findChild<QPushButton*>("checkTikTokViolations");
        auto monitor = settings->findChild<QAbstractButton*>("monitorTikTokViolations");
        if (!Require(safety && safety->isVisible() && monitor && monitor->isChecked(), "Violation controls missing or monitoring not enabled in main OBS settings")) return;
        // Seed only the portable test profile's passive cache. No API call is needed.
        char* cachePath = obs_module_get_config_path(obs_get_module("obs-multi-rtmp"), "rapidapi-quota");
        if (!Require(cachePath != nullptr, "Quota cache directory unavailable")) return;
        const QString directory = QString::fromUtf8(cachePath); bfree(cachePath);
        QDir().mkpath(directory);
        const QByteArray fixtureKey = "quota-frontend-fixture";
        const auto identity = QCryptographicHash::hash(QByteArray("tiktok-live-studio-api-signer1.p.rapidapi.com") + '\0' + fixtureKey, QCryptographicHash::Sha256).toHex();
        QFile cache(QDir(directory).filePath(QString::fromLatin1(identity) + ".json"));
        if (!Require(cache.open(QIODevice::WriteOnly), "Cannot create isolated quota fixture")) return;
        cache.write(QJsonDocument(QJsonObject{{"limit", 10000}, {"remaining", 1000},
            {"reset", QDateTime::currentSecsSinceEpoch() + 3600}, {"updated", QDateTime::currentSecsSinceEpoch()}}).toJson());
        cache.close();
        settings->findChild<QLineEdit*>("rapidApiKey")->setText(QString::fromLatin1(fixtureKey));
        if (!Require(quota->value() == 100 && quota->styleSheet().contains("#d9534f"), "Main OBS quota did not load and render the low-quota cache")) return;
        QApplication::processEvents();
        settings->grab().save(qEnvironmentVariable("TIKTOK_PRIMARY_SMOKE_REPORT") + ".quota.png");
        settings->findChild<QLineEdit*>("rapidApiKey")->clear();
        buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        settings->findChild<QLineEdit*>("cookieFilePath")->setText("F:/Apps/TiktokStreaming/cookies_fixture.json");
        if (!Require(buttons->button(QDialogButtonBox::Apply)->isEnabled(), "Cookie Browse did not enable native Apply")) return;
        if (!Require(settings->findChild<QPushButton*>("tiktokQrLogin") && settings->findChild<QPushButton*>("detectStreamlabsAccount"), "Login controls missing from native settings")) return;
        buttons->button(QDialogButtonBox::Apply)->click();
        OBSService service = obs_frontend_get_streaming_service();
        if (!Require(settings->isVisible() && QString(obs_service_get_id(service)) == "rtmp_custom", "Invalid TikTok credentials should block Apply")) return;
        if (!Require(settings->findChild<QLabel*>("connectionError")->isVisible(), "Required field error is hidden after Apply")) return;
        settings->grab().save(qEnvironmentVariable("TIKTOK_PRIMARY_SMOKE_REPORT") + ".png");
        safety->click(); stage = 5; return;
    }
    if (stage == 5) {
        auto safety = settings->findChild<QDialog*>("tiktokSafetyDialog");
        if (!Require(safety && safety->isVisible() && safety->parentWidget() == settings && !safety->isModal(),
            "Manual violation window must be a modeless child of actual OBS Settings")) return;
        const auto window = reinterpret_cast<HWND>(safety->winId());
        if (!Require(IsWindowEnabled(window) && GetWindow(window, GW_OWNER) == reinterpret_cast<HWND>(settings->winId()),
            "Windows must allow interaction with the violation window above modal OBS Settings")) return;
        auto status = safety->findChild<QLabel*>("tiktokAppealStatus");
        if (!status || !status->text().contains("cookie", Qt::CaseInsensitive)) return;
        safety->grab().save(qEnvironmentVariable("TIKTOK_PRIMARY_SMOKE_REPORT") + ".violation.png");
        safety->reject();
        buttons->button(QDialogButtonBox::Cancel)->click();
        settings = nullptr; stage = 3; return;
    }
    if (stage == 4) {
        if (!Require(provider->currentIndex() == 1, "Managed service did not restore TikTok provider")) return;
        provider->setCurrentIndex(0);
        settings->findChild<QComboBox*>("service")->setCurrentIndex(0);
        settings->findChild<QLineEdit*>("customServer")->setText("rtmp://127.0.0.1/fixture-restored");
        settings->findChild<QLineEdit*>("key")->setText("fixture");
        buttons->button(QDialogButtonBox::Apply)->click();
        OBSService service = obs_frontend_get_streaming_service();
        if (!Require(QString(obs_service_get_id(service)) == "rtmp_custom", "Switching from TikTok did not restore normal OBS service")) return;
        buttons->button(QDialogButtonBox::Ok)->click(); settings = nullptr; Done();
    }
}
void Event(obs_frontend_event event, void*) {
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
    driver = new QTimer(qApp);
    QObject::connect(driver, &QTimer::timeout, [] {
        if (stage == 3) {
            OBSDataAutoRelease data = obs_data_create(); obs_data_set_string(data, "service", "TikTok");
            OBSServiceAutoRelease service = obs_service_create("tiktok_primary_service", "default_service", data, nullptr);
            if (!Require(service != nullptr, "Managed TikTok OBS service was not registered")) return;
            obs_frontend_set_streaming_service(service); obs_frontend_save_streaming_service();
            stage = 4;
            QMetaObject::invokeMethod(static_cast<QWidget*>(obs_frontend_get_main_window()), "on_action_Settings_triggered", Qt::QueuedConnection);
        } else Tick();
    });
    driver->start(150);
}
}
bool obs_module_load() {
    if (qEnvironmentVariableIsEmpty("TIKTOK_PRIMARY_SMOKE_REPORT")) return false;
    obs_frontend_add_event_callback(Event, nullptr); return true;
}
void obs_module_unload() { if (driver) delete driver; }
