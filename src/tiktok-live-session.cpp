// Native OBS packet integration adapted from TikTok Live OBS (GPL-3.0-only).
// See data/licenses/tiktok-live-obs.txt for attribution and source revision.
#include "tiktok-live-session.h"
#include "tiktok-safety.h"
#include "tiktok-native/live_client.hpp"
#include "tiktok-native/sei_metadata.hpp"
#include "tiktok-native/mobile_profile.hpp"
#include "tiktok-native/request_cancellation.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QCryptographicHash>
#include <QSet>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <thread>

namespace {
std::mutex leasesMutex;
QSet<QByteArray> leases;

// Cookie files can be copied/renamed. Reserve the account identifier (or its
// authenticated session when uid_tt is absent), never the file path.
QByteArray AccountKey(const QByteArray& cookies)
{
    for (const auto name : {QByteArray("uid_tt"), QByteArray("sessionid"), QByteArray("sessionid_ss")})
        for (const auto& line : cookies.split('\n')) {
            const auto fields = line.split('\t');
            if (fields.size() >= 7 && fields[5] == name && !fields[6].trimmed().isEmpty())
                return QCryptographicHash::hash(name + ':' + fields[6].trimmed(), QCryptographicHash::Sha256);
        }
    return QCryptographicHash::hash(cookies, QCryptographicHash::Sha256);
}

class NetworkDialog : public QDialog {
    std::atomic_bool* cancelled_;
    QLabel* label_;
    QPushButton* cancel_ = nullptr;
public:
    NetworkDialog(const QString& label, std::atomic_bool* cancelled) :
        QDialog(static_cast<QWidget*>(obs_frontend_get_main_window())), cancelled_(cancelled) {
        setWindowTitle("TikTok"); setWindowModality(Qt::ApplicationModal);
        auto layout = new QVBoxLayout(this);
        label_ = new QLabel(label, this); layout->addWidget(label_);
        if (cancelled_) {
            cancel_ = new QPushButton(obs_module_text("Cancel"), this);
            cancel_->setObjectName("cancelTikTokPreparation"); layout->addWidget(cancel_);
            QObject::connect(cancel_, &QPushButton::clicked, this, [this] { reject(); });
        }
    }
    void reject() override {
        if (cancelled_) { *cancelled_ = true; cancel_->setEnabled(false); label_->setText(obs_module_text("Dual.Cancelling")); }
    }
};
// Keep OBS repainting during HTTP requests. The modal dialog prevents editing
// or deleting the target while Start/Stop holds its configuration.
void WaitForNetwork(const QString &label, const std::function<void()> &work, std::atomic_bool* cancelled = nullptr)
{
    auto result = std::async(std::launch::async, work);
    NetworkDialog progress(label, cancelled);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &progress, [&] {
        if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
    });
    poll.start(50);
    progress.exec();
    result.get();
}
}

struct TikTokLiveSession::State {
    TikTokStudioLive live;
    HostedSigningServiceConfig signer;
    FrameSignInput frameInput;
    struct Feed {
        State* owner;
        std::unique_ptr<SignedSeiSession> metadata;
        obs_output_t* output;
    };
    std::vector<std::unique_ptr<Feed>> feeds;
    QTimer monitor;
    std::unique_ptr<TikTokSafetyMonitor> safety;
    std::atomic_bool cancelled{false}, failed{false}, sending{false};
    std::mutex errorMutex, wakeMutex;
    QString error;
    std::condition_variable wake;
    std::thread heartbeat, signatures;
    bool stopping = false;
    bool adopted = false;
    QByteArray accountLease, roomLease;

    bool Reserve(const QByteArray& key, QByteArray& slot) {
        std::lock_guard lock(leasesMutex);
        if (leases.contains(key)) return false;
        leases.insert(key); slot = key; return true;
    }
    void ReleaseLeases() {
        std::lock_guard lock(leasesMutex);
        if (!accountLease.isEmpty()) leases.remove(accountLease);
        if (!roomLease.isEmpty()) leases.remove(roomLease);
        accountLease.clear(); roomLease.clear();
    }

    void Fail(QString message)
    {
        if (message.isEmpty() || cancelled) return;
        std::lock_guard lock(errorMutex);
        if (error.isEmpty()) error = std::move(message);
        failed = true;
    }
    bool Wait(int seconds)
    {
        std::unique_lock lock(wakeMutex);
        return wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return cancelled.load(); });
    }
    static void Packet(obs_output_t *, encoder_packet *packet, encoder_packet_time *, void *opaque)
    {
        auto &feed = *static_cast<Feed *>(opaque);
        auto &state = *feed.owner;
        if (!packet || !packet->data || packet->type != OBS_ENCODER_VIDEO || packet->track_idx != 0 || state.failed)
            return;
        state.sending = true;
        const char *codec = packet->encoder ? obs_encoder_get_codec(packet->encoder) : nullptr;
        if (!codec || (strcmp(codec, "h264") != 0 && strcmp(codec, "hevc") != 0)) {
            state.Fail("TikTok signing requires an H.264 or HEVC video encoder.");
            return;
        }
        QByteArray replacement;
        QString error;
        const auto original = QByteArray::fromRawData(reinterpret_cast<const char *>(packet->data),
            static_cast<qsizetype>(packet->size));
        if (feed.metadata->transform_packet(strcmp(codec, "hevc") == 0 ? SignedVideoCodec::Hevc : SignedVideoCodec::H264,
                original, packet->dts_usec / 1000, static_cast<int>(obs_encoder_get_width(packet->encoder)),
                static_cast<int>(obs_encoder_get_height(packet->encoder)), QDateTime::currentMSecsSinceEpoch(), &replacement, &error)) {
            const auto size = static_cast<size_t>(replacement.size());
            // OBS stores a long reference count immediately before packet data.
            // A new buffer isolates outputs that share the same encoder.
            auto *references = static_cast<long *>(bmalloc(sizeof(long) + size));
            *references = 1;
            std::memcpy(references + 1, replacement.constData(), size);
            encoder_packet changed = *packet;
            changed.data = reinterpret_cast<uint8_t *>(references + 1);
            changed.size = size;
            obs_encoder_packet_release(packet);
            *packet = changed;
        }
        state.Fail(error);
    }
};

TikTokLiveSession::TikTokLiveSession() : state_(std::make_unique<State>())
{
    QObject::connect(&state_->monitor, &QTimer::timeout, this, [this] {
        // OBS holds a mutex during packet callbacks. Stop only from the UI.
        if (state_->failed) {
            state_->monitor.stop();
            for (const auto& feed : state_->feeds) if (feed->output) {
                obs_output_set_last_error(feed->output, Error().toUtf8().constData());
                obs_output_force_stop(feed->output);
            }
        }
    });
}

TikTokLiveSession::~TikTokLiveSession() { Stop(); }

bool TikTokLiveSession::Start(const OutputTargetConfig &config, obs_output_t *output,
    std::function<void(const nlohmann::json &)> saveDevices)
{
    return Start(config, std::vector<obs_output_t*>{output}, std::move(saveDevices));
}

bool TikTokLiveSession::Start(const OutputTargetConfig &config, const std::vector<obs_output_t*>& outputs,
    std::function<void(const nlohmann::json &)> saveDevices)
{
    auto &s = *state_;
    if (outputs.size() != (config.UsesDualLayout() ? 2u : 1u) || !s.feeds.empty() || s.stopping) {
        s.Fail("Invalid TikTok output session."); return false;
    }
    for (auto output : outputs) {
        auto encoder = output ? obs_output_get_video_encoder(output) : nullptr;
        const char *codec = encoder ? obs_encoder_get_codec(encoder) : nullptr;
        if (!codec || (strcmp(codec, "h264") != 0 && strcmp(codec, "hevc") != 0)) {
            s.Fail("Choose an H.264 or HEVC encoder for each TikTok output.");
            return false;
        }
    }
    if (outputs.size() == 2 && outputs[0] == outputs[1]) {
        s.Fail("Dual layout requires distinct outputs.");
        return false;
    }
    auto &account = s.live.account;
    account.rapidapi_key = QString::fromStdString(config.rapidApiKey);
    account.mobile = config.tiktokClient == "mobile";
    const auto devices = config.tiktokDeviceProfiles.is_object() ? config.tiktokDeviceProfiles : nlohmann::json::object();
    const auto profile = devices.contains(config.tiktokClient) && devices[config.tiktokClient].is_object()
        ? devices[config.tiktokClient] : nlohmann::json::object();
    account.device_profile = QJsonDocument::fromJson(QByteArray::fromStdString(profile.dump())).object();
    account.device_id = account.device_profile.value("device_id").toString();
    account.install_id = account.device_profile.value("install_id").toString();
    account.live_studio_version = account.device_profile.value("live_studio_version").toString("1.27.0");
    QString cookieError;
    account.cookie_jar = read_tiktok_cookie_file(QString::fromStdString(config.cookieFilePath), &cookieError);
    if (!cookieError.isEmpty()) { s.Fail(cookieError); return false; }
    s.signer.api_key = account.rapidapi_key;
    if (!s.signer.valid()) { s.Fail("Enter a valid RapidAPI key."); return false; }
    if (!s.Reserve(AccountKey(account.cookie_jar), s.accountLease)) {
        s.Fail("This TikTok account already has a starting or running target."); return false;
    }
    try {
        WaitForNetwork(obs_module_text("Connection.PreparingLive"), [&] {
            TikTokRequestCancellation cancellation(&s.cancelled);
            const auto error = initialize_tiktok_account(account);
            if (!error.isEmpty()) { s.Fail(error); return; }
            QString startError;
            s.live = create_tiktok_live(account, QString::fromStdString(config.streamlabsTitle.empty() ? config.name : config.streamlabsTitle),
                QString::fromStdString(config.tiktokTopicId), QString::fromStdString(config.tiktokGameId), config.streamlabsMatureContent,
                &startError, config.UsesDualLayout(), config.tiktokLiveSettings);
            if (!startError.isEmpty()) { s.Fail(startError); return; }
            if (!s.Reserve("room:" + s.live.room_id.toUtf8(), s.roomLease)) {
                s.live.created_by_us = false;
                s.Fail("This TikTok room is already used by another target."); return;
            }
            s.frameInput.aid = account.mobile ? "1233" : "8311";
            s.frameInput.mobile = account.mobile;
            s.frameInput.sdk_context = mobile_frame_context(account.device_profile);
            s.frameInput.device_id = account.device_id;
            s.frameInput.uid = s.live.owner_user_id;
            s.frameInput.room_id = s.live.room_id;
            SignedSeiConfig sei{s.frameInput.aid, s.frameInput.uid, s.frameInput.device_id, s.frameInput.room_id, account.mobile};
            auto batch = FrameSignClient::fetch_batch(s.signer, s.frameInput, QDateTime::currentSecsSinceEpoch(), 300, 1, &s.cancelled);
            if (!batch.valid()) { s.Fail(batch.error); return; }
            for (auto output : outputs) {
                auto feed = std::make_unique<State::Feed>();
                feed->owner = &s;
                feed->output = output;
                feed->metadata = std::make_unique<SignedSeiSession>(sei);
                feed->metadata->merge_signatures(batch.signatures);
                s.feeds.push_back(std::move(feed));
            }
        }, &s.cancelled);
    } catch (const std::exception &error) { s.Fail("TikTok session preparation failed: " + QString::fromUtf8(error.what())); }
    if (account.has_device() || (account.mobile && !account.device_profile.isEmpty())) {
        auto saved = devices;
        auto updated = nlohmann::json::parse(QJsonDocument(account.device_profile).toJson(QJsonDocument::Compact).constData());
        updated["device_id"] = account.device_id.toStdString();
        updated["install_id"] = account.install_id.toStdString();
        if (!account.mobile) updated["live_studio_version"] = account.live_studio_version.toStdString();
        saved[config.tiktokClient] = updated;
        if (saveDevices) saveDevices(saved);
    }
    if (s.cancelled) {
        std::lock_guard lock(s.errorMutex);
        s.error = obs_module_text("Dual.Cancelled"); s.failed = true;
    }
    if (s.failed) { Stop(); return false; }
    if (config.monitorTikTokViolations) {
        auto context = MakeTikTokSafetyContext(config);
        context.account = s.live.account; context.userId = s.live.owner_user_id;
        s.safety = std::make_unique<TikTokSafetyMonitor>(std::move(context));
    }
    for (const auto& feed : s.feeds) obs_output_add_packet_callback(feed->output, State::Packet, feed.get());
    s.monitor.start(250);
    s.heartbeat = std::thread([&s] {
        TikTokRequestCancellation cancel(&s.cancelled);
        int failures = 0;
        while (!s.Wait(10)) {
            try {
                auto error = ping_tiktok_live(s.live, s.sending ? 2 : 1);
                if (error.isEmpty()) failures = 0;
                else if (++failures >= 3) { s.Fail("TikTok heartbeat failed: " + error); return; }
            } catch (...) { s.Fail("TikTok heartbeat failed."); return; }
        }
    });
    s.signatures = std::thread([&s] {
        qint64 nextMobile = QDateTime::currentSecsSinceEpoch() + 15;
        while (!s.Wait(1)) {
            const auto now = QDateTime::currentSecsSinceEpoch();
            if (s.frameInput.mobile ? now < nextMobile : s.feeds.front()->metadata->cache_last_timestamp() > now + 60) continue;
            try {
                const auto batch = FrameSignClient::fetch_batch(s.signer, s.frameInput, now, 300, 1, &s.cancelled);
                if (batch.valid()) {
                    for (const auto& feed : s.feeds) feed->metadata->merge_signatures(batch.signatures);
                    nextMobile = QDateTime::currentSecsSinceEpoch() + 15;
                } else {
                    if (s.feeds.front()->metadata->cache_last_timestamp() < now - (s.frameInput.mobile ? 25 : 0)) {
                        s.Fail(batch.error); return;
                    }
                    if (s.Wait(5)) return;
                }
            } catch (...) { s.Fail("TikTok frame signing failed."); return; }
        }
    });
    return true;
}

void TikTokLiveSession::Stop()
{
    auto &s = *state_;
    if (s.stopping) return;
    s.stopping = true;
    s.monitor.stop();
    if (s.safety) {
        if (s.adopted) FinishTikTokSafety(std::move(s.safety));
        else s.safety.reset();
    }
    for (const auto& feed : s.feeds) if (feed->output) {
        obs_output_remove_packet_callback(feed->output, State::Packet, feed.get());
        feed->output = nullptr;
    }
    s.cancelled = true;
    s.wake.notify_all();
    if (!s.heartbeat.joinable() && !s.signatures.joinable() && s.live.room_id.isEmpty()) { s.ReleaseLeases(); return; }
    try {
        WaitForNetwork(obs_module_text("Connection.EndingLive"), [&] {
            if (s.heartbeat.joinable()) s.heartbeat.join();
            if (s.signatures.joinable()) s.signatures.join();
            auto error = (s.live.created_by_us || s.adopted) ? end_tiktok_live(s.live) : QString();
            if (!error.isEmpty()) {
                std::lock_guard lock(s.errorMutex);
                s.error += (s.error.isEmpty() ? QString() : "\n") +
                    QStringLiteral("Could not confirm that TikTok ended the LIVE. Check TikTok. ") + error;
            }
        });
    } catch (...) {
        if (s.heartbeat.joinable()) s.heartbeat.join();
        if (s.signatures.joinable()) s.signatures.join();
    }
    s.ReleaseLeases();
}

QString TikTokLiveSession::Error() const
{
    std::lock_guard lock(state_->errorMutex);
    auto error = state_->error;
    if (!state_->signer.api_key.isEmpty()) error.replace(state_->signer.api_key, "<redacted>");
    return error;
}
void TikTokLiveSession::ConfirmStarted() {
    if (!state_->adopted && state_->safety) state_->safety->Start();
    state_->adopted = true;
}
void TikTokLiveSession::CancelPreparation() { state_->cancelled = true; state_->wake.notify_all(); }
std::string TikTokLiveSession::Server(bool landscape) const { return (landscape ? state_->live.dual_server : state_->live.server).toStdString(); }
std::string TikTokLiveSession::Key(bool landscape) const { return (landscape ? state_->live.dual_key : state_->live.key).toStdString(); }
