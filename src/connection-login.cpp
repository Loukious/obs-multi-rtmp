#include "connection-login.h"
#include "streamlabs-desktop.h"
#include "tiktok-native/qr_login.hpp"
#include "tiktok-native/live_client.hpp"
#include "tiktok-native/request_cancellation.hpp"
#include <obs-module.h>
#include <util/bmem.h>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QPixmap>
#include <future>

namespace {
class QrLoginDialog final : public QDialog {
    QLabel *picture, *status;
    QPushButton *retry;
    QDialogButtonBox* buttons;
    QTimer watcher, poll;
    std::future<TikTokQrLogin> work;
    std::atomic_bool cancelled{false};
    bool closing = false;
    int pollFailures = 0;
    QString cookieDirectory;
    void Start(bool fresh) {
        if (work.valid() || closing) return;
        poll.stop(); retry->hide();
        if (fresh) { pollFailures = 0; picture->clear(); picture->setText(obs_module_text("Login.Preparing")); status->clear(); }
        auto input = login;
        work = std::async(std::launch::async, [this, input, fresh]() mutable {
            TikTokRequestCancellation scope(&cancelled);
            try {
                auto result = fresh ? begin_tiktok_qr_login(input.account) : poll_tiktok_qr_login(std::move(input));
                if (result.error.isEmpty() && result.state == TikTokQrState::Confirmed &&
                    result.account.username.trimmed().isEmpty() && !tiktok_cancel_requested()) {
                    // Naming is optional metadata: an unavailable account
                    // lookup must not discard an already confirmed session.
                    try {
                        QString error;
                        result.account.username = lookup_tiktok_username(result.account, &error);
                    } catch (const std::exception&) {}
                }
                return result;
            }
            catch (const std::exception& error) { input.error = QString::fromUtf8(error.what()); input.retryable = false; return input; }
        });
    }
    void Failed(const QString& error) { status->setText(error); picture->clear(); retry->show(); }
    void Receive() {
        if (!work.valid() || work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        login = work.get();
        if (closing) { QDialog::reject(); return; }
        if (!login.error.isEmpty()) {
            if (login.retryable && !login.token.isEmpty() && ++pollFailures <= 6) {
                const int delay = pollFailures == 1 ? 5000 : pollFailures == 2 ? 10000 : pollFailures == 3 ? 20000 : 30000;
                status->setText(QString::fromUtf8(obs_module_text("Login.Retrying")).arg(delay / 1000));
                retry->show();
                poll.start(delay);
                return;
            }
            // Like LIVE Studio, replace a code that can no longer be checked.
            // A failure to generate the replacement is terminal, so a signer
            // outage cannot turn into an unbounded generation retry loop.
            if (login.retryable && !login.token.isEmpty()) { Start(true); return; }
            Failed(login.error); return;
        }
        pollFailures = 0;
        if (login.state == TikTokQrState::Confirmed) {
            QString error;
            cookiePath = save_tiktok_login_cookies(login.account.cookie_jar, cookieDirectory, &error,
                login.account.username.isEmpty() ? login.account.user_id : login.account.username);
            if (cookiePath.isEmpty()) { Failed(error); return; }
            accept(); return;
        }
        if (login.state == TikTokQrState::Expired || login.state == TikTokQrState::Refused || login.state == TikTokQrState::Error) {
            Start(true); return;
        }
        QPixmap image;
        if (!image.loadFromData(login.png, "PNG")) { Failed(obs_module_text("Login.InvalidQr")); return; }
        picture->setPixmap(image.scaled(280, 280, Qt::KeepAspectRatio, Qt::FastTransformation));
        status->setText(obs_module_text(login.state == TikTokQrState::Scanned ? "Login.Confirm" : "Login.Scan"));
        poll.start(2500);
    }
public:
    TikTokQrLogin login;
    QString cookiePath;
    QrLoginDialog(TikTokStudioAccountCredentials account, QString directory, QWidget* parent)
        : QDialog(parent), cookieDirectory(std::move(directory)) {
        login.account = std::move(account);
        setWindowTitle(obs_module_text("Login.QrTitle")); setModal(true); resize(400, 430);
        auto layout = new QVBoxLayout(this);
        picture = new QLabel(this); picture->setAlignment(Qt::AlignCenter); picture->setMinimumSize(280, 280); layout->addWidget(picture);
        status = new QLabel(this); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
        retry = new QPushButton(obs_module_text("Login.NewQr"), this); retry->setAutoDefault(false); layout->addWidget(retry);
        picture->setObjectName("qrPicture"); status->setObjectName("qrStatus"); retry->setObjectName("qrRegenerate");
        poll.setObjectName("qrPollTimer"); poll.setParent(this);
        buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QrLoginDialog::reject);
        connect(retry, &QPushButton::clicked, this, [this] { Start(true); });
        connect(&watcher, &QTimer::timeout, this, [this] { Receive(); }); watcher.start(50);
        poll.setInterval(2500); poll.setSingleShot(true); connect(&poll, &QTimer::timeout, this, [this] { Start(false); });
        QTimer::singleShot(0, this, [this] { Start(true); });
    }
    ~QrLoginDialog() override { cancelled = true; if (work.valid()) work.wait(); }
    void reject() override {
        closing = true; cancelled = true; poll.stop(); picture->clear(); retry->hide();
        if (work.valid()) { status->setText(obs_module_text("Login.Cancelling")); buttons->setEnabled(false); }
        else QDialog::reject();
    }
};
class DetectDialog final : public QDialog {
    std::atomic_bool cancelled{false};
    std::future<QString> work;
    QTimer timer;
    bool closing = false;
public:
    QString token;
    explicit DetectDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle(obs_module_text("Login.DetectStreamlabs"));
        auto layout = new QVBoxLayout(this); layout->addWidget(new QLabel(obs_module_text("Login.Detecting"), this));
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &DetectDialog::reject);
        work = std::async(std::launch::async, [this] { return FindStreamlabsDesktopToken(StreamlabsDesktopDirectory(), cancelled); });
        connect(&timer, &QTimer::timeout, this, [this] {
            if (work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            timer.stop();
            try { token = work.get(); } catch (...) { token.clear(); }
            if (closing) QDialog::reject(); else accept();
        }); timer.start(50);
    }
    ~DetectDialog() override { cancelled = true; if (work.valid()) work.wait(); }
    void reject() override { closing = true; cancelled = true; if (!work.valid()) QDialog::reject(); }
};
}

bool LoginTikTokWithQr(QWidget* parent, OutputTargetConfig& config)
{
    if (QString::fromStdString(config.rapidApiKey).trimmed().isEmpty()) {
        QMessageBox::information(parent, obs_module_text("Login.QrTitle"), obs_module_text("Login.NeedKey")); return false;
    }
    TikTokStudioAccountCredentials account;
    account.rapidapi_key = QString::fromStdString(config.rapidApiKey);
    if (config.tiktokDeviceProfiles.is_object() && config.tiktokDeviceProfiles.contains("pc"))
        account.device_profile = QJsonDocument::fromJson(QByteArray::fromStdString(config.tiktokDeviceProfiles["pc"].dump())).object();
    account.device_id = account.device_profile.value("device_id").toString();
    account.install_id = account.device_profile.value("install_id").toString();
    account.live_studio_version = account.device_profile.value("live_studio_version").toString("1.27.0");
    char* path = obs_module_config_path("cookies");
    const QString directory = path ? QString::fromUtf8(path) : QString(); bfree(path);
    if (directory.isEmpty()) { QMessageBox::warning(parent, obs_module_text("Login.QrTitle"), obs_module_text("Login.SaveFailed")); return false; }
    QrLoginDialog dialog(account, directory, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    config.cookieFilePath = dialog.cookiePath.toStdString();
    auto& profile = config.tiktokDeviceProfiles["pc"];
    if (!profile.is_object()) profile = nlohmann::json::object();
    profile["device_id"] = dialog.login.account.device_id.toStdString();
    profile["install_id"] = dialog.login.account.install_id.toStdString();
    profile["live_studio_version"] = dialog.login.account.live_studio_version.toStdString();
    return true;
}
QString DetectStreamlabsDesktopAccount(QWidget* parent)
{
    DetectDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted) return {};
    if (dialog.token.isEmpty()) QMessageBox::information(parent, obs_module_text("Login.DetectStreamlabs"), obs_module_text("Login.NotFound"));
    return dialog.token;
}
