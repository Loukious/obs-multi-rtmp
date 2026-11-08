#include "primary-settings.h"
#include "primary-output.h"
#include "primary-canvas.h"
#include "connection-settings.h"
#include "dual-layout-widget.h"
#include "layout-scene.h"
#include "edit-widget.h"
#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <QApplication>
#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPointer>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTimer>
#include <QCloseEvent>

namespace {
class SettingsBridge : public QObject {
    QDialog* dialog;
    PrimarySettingsHost host;
    QDialogButtonBox* buttons;
    QComboBox* provider;
    QWidget* original;
    QTabWidget* tabs;
    ConnectionSettingsWidget* connection;
    QLabel* error;
    OutputTargetConfig config;
    std::string saved;
    bool savedEnabled = false, loading = true;

    void Visibility() {
        original->setVisible(provider->currentIndex() == 0);
        tabs->setVisible(provider->currentIndex() == 1);
    }
    void Changed() {
        if (loading || host.busy()) return;
        auto current = config; connection->Save(current);
        if (EncodePrimaryConfig(current) != saved || (provider->currentIndex() == 1) != savedEnabled)
            buttons->button(QDialogButtonBox::Apply)->setEnabled(true);
    }
    void RestorePrevious() {
        if (!host.previousService.is_object()) return;
        auto fields = host.previousService.value("settings", nlohmann::json::object());
        auto service = dialog->findChild<QComboBox*>("service");
        const auto id = host.previousService.value("type", std::string("rtmp_custom"));
        const auto name = fields.value("service", std::string());
        if (id == "rtmp_custom") service->setCurrentIndex(0);
        else {
            int index = service->findText(QString::fromStdString(id == "whip_custom" ? "WHIP" : name));
            if (index < 0 && !name.empty()) { service->insertItem(1, QString::fromStdString(name)); index = 1; }
            if (index >= 0) service->setCurrentIndex(index);
        }
        for (auto [widget, key] : {std::pair{"customServer", "server"}, {"key", "key"}, {"authUsername", "username"}, {"authPw", "password"}})
            if (auto edit = dialog->findChild<QLineEdit*>(widget); edit && fields.contains(key) && fields[key].is_string()) edit->setText(QString::fromStdString(fields[key].get<std::string>()));
        if (auto auth = dialog->findChild<QCheckBox*>("useAuth")) auth->setChecked(fields.value("use_auth", false));
        if (auto server = dialog->findChild<QComboBox*>("server")) {
            const int index = server->findData(QString::fromStdString(fields.value("server", std::string())));
            if (index >= 0) server->setCurrentIndex(index);
        }
    }
    void Click(QAbstractButton* button) {
        const auto role = buttons->buttonRole(button);
        if (role != QDialogButtonBox::ApplyRole && role != QDialogButtonBox::AcceptRole) {
            QMetaObject::invokeMethod(dialog, "on_buttonBox_clicked", Qt::DirectConnection, Q_ARG(QAbstractButton*, button)); return;
        }
        const bool enabled = provider->currentIndex() == 1;
        auto next = config; connection->Save(next);
        const auto beforePrepare = next;
        error->hide();
        if (!host.busy() && enabled) {
            if (!connection->Validate()) {
                return;
            }
            QString reason;
            if (!host.prepare(next, &reason)) { connection->LayoutSettings()->ShowVideoError(reason); return; }
            // Route OBS's own service-change/auth cleanup through its normal
            // form before installing the prepared managed service below.
            dialog->findChild<QComboBox*>("service")->setCurrentIndex(0);
            dialog->findChild<QLineEdit*>("customServer")->setText("rtmp://localhost/tiktok-managed");
            dialog->findChild<QLineEdit*>("key")->setText("managed");
        }
        // OBS's slot returns void and may reject invalid native encoder or
        // recording settings. Apply first; ClearChanged disables Apply only
        // after native validation and saving succeeded. Close after commit.
        auto apply = buttons->button(QDialogButtonBox::Apply);
        apply->setEnabled(true);
        QPointer<SettingsBridge> guard(this);
        QMetaObject::invokeMethod(dialog, "on_buttonBox_clicked", Qt::DirectConnection, Q_ARG(QAbstractButton*, static_cast<QAbstractButton*>(apply)));
        if (!guard) return;
        if (apply->isEnabled()) {
            if (host.rollbackPrepare) host.rollbackPrepare(beforePrepare, next);
            return;
        }
        if (!host.busy()) {
            host.commit(next, enabled);
            config = next; saved = EncodePrimaryConfig(next); savedEnabled = enabled;
            loading = true; connection->Load(config); connection->LayoutSettings()->SetNativeCanvasMode(true); loading = false;
        }
        if (role == QDialogButtonBox::AcceptRole) dialog->close();
    }
public:
    SettingsBridge(QDialog* window, PrimarySettingsHost callbacks, QDialogButtonBox* box, QWidget* streamPage)
        : QObject(window), dialog(window), host(std::move(callbacks)), buttons(box) {
        config = host.load(); saved = EncodePrimaryConfig(config); savedEnabled = host.enabled();
        auto nativeLayout = streamPage->layout();
        const auto pageMargins = nativeLayout->contentsMargins();
        const auto pageSpacing = nativeLayout->spacing();
        auto nativeForm = streamPage->findChild<QFormLayout*>("topStreamLayout");
        auto serviceLabel = streamPage->findChild<QLabel*>("serviceLabel");
        original = new QWidget(streamPage); original->setObjectName("originalOBSStreamSettings");
        original->setLayout(nativeLayout);
        // Keep OBS's page inset around both providers, without applying it
        // twice to the original service form.
        nativeLayout->setContentsMargins(0, 0, 0, 0);
        auto layout = new QVBoxLayout(streamPage);
        layout->setContentsMargins(pageMargins);
        layout->setSpacing(pageSpacing);
        auto form = new QFormLayout;
        const auto formMargins = nativeForm ? nativeForm->contentsMargins() : QMargins();
        form->setContentsMargins(formMargins.left(), formMargins.top(), formMargins.right(), 0);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (nativeForm) form->setHorizontalSpacing(nativeForm->horizontalSpacing());
        provider = new QComboBox(streamPage); provider->setObjectName("primaryStreamingProvider");
        provider->addItem(obs_module_text("Primary.OBSService")); provider->addItem("TikTok");
        provider->setCurrentIndex(savedEnabled ? 1 : 0);
        auto providerLabel = new QLabel(obs_module_text("Primary.Provider"), streamPage);
        providerLabel->setObjectName("primaryStreamingProviderLabel");
        providerLabel->setAlignment(serviceLabel ? serviceLabel->alignment() : Qt::AlignRight | Qt::AlignVCenter);
        if (serviceLabel) providerLabel->setMinimumWidth(serviceLabel->minimumWidth());
        providerLabel->setBuddy(provider);
        form->addRow(providerLabel, provider); layout->addLayout(form);
        layout->addWidget(original);
        tabs = new QTabWidget(streamPage); tabs->setObjectName("primaryTikTokTabs"); layout->addWidget(tabs, 1);
        connection = new ConnectionSettingsWidget(new QWidget, tabs);
        connection->setObjectName("primaryTikTokConnection");
        auto method = connection->findChild<QComboBox*>("connectionMethod");
        static_cast<QStandardItemModel*>(method->model())->item(0)->setEnabled(false);
        auto page = new QWidget(tabs); auto pageLayout = new QVBoxLayout(page); pageLayout->setAlignment(Qt::AlignTop);
        pageLayout->addWidget(connection);
        error = new QLabel(page); error->setObjectName("primaryTikTokError"); error->setWordWrap(true); error->setTextFormat(Qt::PlainText); error->hide(); pageLayout->addWidget(error);
        auto scroll = new QScrollArea(tabs); scroll->setWidgetResizable(true); scroll->setWidget(page); tabs->addTab(scroll, obs_module_text("Tab.Service"));
        for (auto [content, title] : {std::pair{connection->LayoutSettings()->VideoSettings(), "VideoSettings"},
            std::pair{connection->LayoutSettings()->AudioSettings(), "AudioSettings"}}) {
            auto inner = new QWidget(tabs); auto row = new QVBoxLayout(inner); row->setAlignment(Qt::AlignTop);
            auto note = new QLabel(obs_module_text("Primary.SingleSettings"), inner); note->setWordWrap(true); row->addWidget(note); row->addWidget(content);
            auto scroll = new QScrollArea(tabs); scroll->setWidgetResizable(true); scroll->setWidget(inner); tabs->addTab(scroll, obs_module_text(title));
        }
        connection->SetLayoutPreview([this](const LayoutVideoConfig& value) { ShowLayoutPreview(value, dialog); });
        connection->LayoutSettings()->editEncoder = [this](const std::string& id, nlohmann::json& params, int& bitrate, bool audio) { return EditLayoutEncoder(dialog, id, params, bitrate, audio); };
        connection->Load(config); connection->LayoutSettings()->SetNativeCanvasMode(true);
        connection->SetContentChangedCallback([this] { Changed(); });
        // File pickers, account import and QR login use setText(), which does
        // not emit textEdited. They must mark Apply dirty just like typing.
        for (auto field : tabs->findChildren<QLineEdit*>()) QObject::connect(field, &QLineEdit::textChanged, this, [this] { Changed(); });
        for (auto field : tabs->findChildren<QSpinBox*>()) QObject::connect(field, &QSpinBox::valueChanged, this, [this] { Changed(); });
        for (auto field : tabs->findChildren<QComboBox*>()) QObject::connect(field, &QComboBox::activated, this, [this] { Changed(); });
        for (auto field : tabs->findChildren<QCheckBox*>()) QObject::connect(field, &QCheckBox::clicked, this, [this] { Changed(); });
        QObject::connect(provider, &QComboBox::currentIndexChanged, this, [this] { Visibility(); if (provider->currentIndex() == 0 && savedEnabled) RestorePrevious(); Changed(); });
        QObject::connect(buttons, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) { Click(button); });
        provider->setEnabled(!host.busy()); tabs->setEnabled(!host.busy());
        loading = false; Visibility();
    }
};

PrimarySettingsHost MakeHost() {
    PrimarySettingsHost host;
    host.load = [] {
        OBSService service = obs_frontend_get_streaming_service();
        OBSDataAutoRelease settings = obs_service_get_settings(service);
        const char* saved = service && std::string(obs_service_get_id(service)) == TikTokPrimaryServiceId
            ? obs_data_get_string(settings, "tiktok_config") : config_get_string(obs_frontend_get_profile_config(), "TikTok", "PrimaryConfig");
        return DecodePrimaryConfig(saved);
    };
    host.enabled = [] { OBSService service = obs_frontend_get_streaming_service(); return service && std::string(obs_service_get_id(service)) == TikTokPrimaryServiceId; };
    host.busy = [] { return obs_frontend_streaming_active() || PrimaryTikTokBusy(); };
    OBSService service = obs_frontend_get_streaming_service();
    if (service && std::string(obs_service_get_id(service)) != TikTokPrimaryServiceId) {
        OBSDataAutoRelease settings = obs_service_get_settings(service);
        host.previousService = {{"type", obs_service_get_id(service)}, {"settings", nlohmann::json::parse(obs_data_get_json(settings))}};
    } else {
        const char* previous = config_get_string(obs_frontend_get_profile_config(), "TikTok", "PreviousService");
        host.previousService = nlohmann::json::parse(previous ? previous : "", nullptr, false);
        if (host.previousService.is_discarded()) host.previousService = nullptr;
    }
    host.prepare = [](OutputTargetConfig& config, QString* error) { return PreparePrimaryCanvases(config, error); };
    host.rollbackPrepare = [](const OutputTargetConfig& before, const OutputTargetConfig& after) {
        RemoveReplacedPrimaryCanvases(after, before);
    };
    const auto backup = host.previousService;
    const auto load = host.load;
    host.commit = [backup, load](const OutputTargetConfig& config, bool enabled) {
        const auto previous = load();
        auto profile = obs_frontend_get_profile_config();
        const auto json = EncodePrimaryConfig(config);
        config_set_string(profile, "TikTok", "PrimaryConfig", json.c_str());
        if (enabled) {
            if (backup.is_object()) config_set_string(profile, "TikTok", "PreviousService", backup.dump().c_str());
            OBSDataAutoRelease settings = obs_data_create();
            obs_data_set_string(settings, "service", "TikTok"); obs_data_set_string(settings, "tiktok_config", json.c_str());
            OBSServiceAutoRelease service = obs_service_create(TikTokPrimaryServiceId, "default_service", settings, nullptr);
            obs_frontend_set_streaming_service(service); obs_frontend_save_streaming_service();
            RemoveReplacedPrimaryCanvases(previous, config);
            obs_frontend_save();
        }
        config_save_safe(profile, "tmp", nullptr);
    };
    return host;
}
class SettingsWatcher : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Close && watched == obs_frontend_get_main_window() && PrimaryTikTokBusy()) {
            static_cast<QCloseEvent*>(event)->ignore();
            QPointer<QWidget> window = qobject_cast<QWidget*>(watched);
            // Let in-flight preparation unwind before the frontend destroys
            // libobs. No user dialog and no process termination are needed.
            auto retry = new QTimer(this);
            QObject::connect(retry, &QTimer::timeout, retry, [window, retry] {
                if (!PrimaryTikTokBusy()) { retry->stop(); retry->deleteLater(); if (window) window->close(); }
            });
            retry->start(50); StopPrimaryTikTokOutputs();
            return true;
        }
        if (event->type() == QEvent::Show) if (auto dialog = qobject_cast<QDialog*>(watched);
            dialog && QByteArray(dialog->metaObject()->className()) == "OBSBasicSettings" && !dialog->property("tiktokPrimaryAttached").toBool()) {
            if (AttachPrimaryTikTokSettings(dialog, MakeHost())) dialog->setProperty("tiktokPrimaryAttached", true);
            else blog(LOG_WARNING, "[obs-multi-rtmp] Could not attach TikTok to this OBS Settings dialog.");
        }
        return false;
    }
};
QPointer<SettingsWatcher> watcher;
void FrontendEvent(obs_frontend_event event, void*) {
    if (event == OBS_FRONTEND_EVENT_EXIT) { ShutdownPrimaryTikTokFrontend(); return; }
    if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING)
        StopPrimaryTikTokOutputs();
}
}

bool AttachPrimaryTikTokSettings(QDialog* dialog, PrimarySettingsHost host) {
    auto page = dialog->findChild<QWidget*>("streamPage"); auto buttons = dialog->findChild<QDialogButtonBox*>("buttonBox");
    if (!page || !page->layout() || !buttons || !buttons->button(QDialogButtonBox::Apply) ||
        !dialog->findChild<QComboBox*>("service") || !dialog->findChild<QLineEdit*>("customServer") || !dialog->findChild<QLineEdit*>("key") ||
        dialog->metaObject()->indexOfSlot("on_buttonBox_clicked(QAbstractButton*)") < 0) return false;
    // Adapt only this known OBS slot. All remaining OBS signal connections stay intact.
    if (!QObject::disconnect(buttons, SIGNAL(clicked(QAbstractButton*)), dialog, SLOT(on_buttonBox_clicked(QAbstractButton*)))) return false;
    new SettingsBridge(dialog, std::move(host), buttons, page);
    return true;
}
void InstallPrimaryTikTokFrontend() {
    watcher = new SettingsWatcher(qApp); qApp->installEventFilter(watcher);
    obs_frontend_add_event_callback(FrontendEvent, nullptr);
    SetPrimaryServiceSaveCallback([](obs_service_t* service) {
        OBSService current = obs_frontend_get_streaming_service();
        if (current == service) obs_frontend_save_streaming_service();
    });
}
void ShutdownPrimaryTikTokFrontend() {
    StopPrimaryTikTokOutputs(); SetPrimaryServiceSaveCallback({});
    obs_frontend_remove_event_callback(FrontendEvent, nullptr);
    delete watcher;
}
