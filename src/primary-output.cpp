#include "primary-output.h"
#include "dual-output-session.h"
#include "connection-settings.h"
#include "tiktok-live-session.h"
#include "streamlabs-api.h"
#include "tiktok-native/request_cancellation.hpp"
#include <obs.hpp>
#include <obs-module.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <atomic>
#include <future>
#include <mutex>

std::string EncodePrimaryConfig(const OutputTargetConfig& config) {
    MultiOutputConfig saved; saved.targets.push_back(std::make_shared<OutputTargetConfig>(config));
    return SerializeMultiOutputConfig(saved);
}
OutputTargetConfig DecodePrimaryConfig(const char* json) {
    auto saved = DeserializeMultiOutputConfig(json ? json : "");
    OutputTargetConfig result;
    result.id = "tiktok-primary"; result.name = "TikTok";
    result.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    if (saved && saved->targets.size() == 1) result = *saved->targets.front();
    result.id = "tiktok-primary"; result.syncStart = result.syncStop = false;
    return result;
}

namespace {
std::function<void(obs_service_t*)> saveService;
std::mutex instancesMutex, statusMutex;
QString latestStatus;
class PrimaryOutput;
QSet<PrimaryOutput*> instances;
void SetStatus(const QString& value) { std::lock_guard lock(statusMutex); latestStatus = value; }

// OBS owns the frontend controls and this registered output. The transport
// outputs share its selected encoder(s); no extra encoder runs just for status.
class PrimaryOutput : public QObject {
public:
    obs_output_t* raw;
    OBSOutput hold;
    std::atomic_bool pending{false}, cancelled{false};
    std::atomic_uint64_t bytes{0};
    std::atomic_int dropped{0};
    bool preparing = false, stopping = false, captured = false, finishing = false, reconnectReported = false;
    QTimer* timer = nullptr;
    QElapsedTimer startup;
    std::unique_ptr<DualOutputSession> dual;
    std::unique_ptr<TikTokLiveSession> native;
    OBSOutput child;
    OBSService childService;
    std::atomic_bool childStarted{false}, childStopped{false}, childReconnecting{false};
    std::atomic_int childStopCode{OBS_OUTPUT_SUCCESS};
    bool childRequested = false;
    OutputTargetConfig config;
    QString error;
    std::string streamlabsId;

    explicit PrimaryOutput(obs_output_t* output) : raw(output) {
        moveToThread(qApp->thread());
        std::lock_guard lock(instancesMutex); instances.insert(this);
    }
    ~PrimaryOutput() override { std::lock_guard lock(instancesMutex); instances.remove(this); }

    static void Started(void* p, calldata_t*) { auto self = static_cast<PrimaryOutput*>(p); self->childStarted = true; self->childReconnecting = false; }
    static void Reconnecting(void* p, calldata_t*) { static_cast<PrimaryOutput*>(p)->childReconnecting = true; }
    static void Stopped(void* p, calldata_t* data) {
        auto self = static_cast<PrimaryOutput*>(p);
        self->childStopCode = static_cast<int>(calldata_int(data, "code")); self->childStopped = true;
    }

    void SaveDevices(const nlohmann::json& profiles) {
        config.tiktokDeviceProfiles = profiles;
        auto service = obs_output_get_service(hold);
        OBSDataAutoRelease settings = obs_service_get_settings(service);
        obs_data_set_string(settings, "tiktok_config", EncodePrimaryConfig(config).c_str());
        obs_service_update(service, settings);
        if (saveService) saveService(service);
    }

    void Start() {
        error.clear(); bytes = 0; dropped = 0; stopping = captured = finishing = false;
        childStarted = false; childStopped = false; childRequested = false; childReconnecting = false; reconnectReported = false;
        childStopCode = OBS_OUTPUT_SUCCESS;
        if (cancelled) { Finish(); return; }
        OBSDataAutoRelease serviceSettings = obs_service_get_settings(obs_output_get_service(hold));
        config = DecodePrimaryConfig(obs_data_get_string(serviceSettings, "tiktok_config"));
        if (const auto invalid = ValidateConnectionSettings(config)) { error = obs_module_text(invalid); Finish(); return; }
        if (config.connectionMethod == ConnectionMethod::Custom) { error = "Choose a TikTok connection method."; Finish(); return; }
        SetStatus(obs_module_text("Connection.PreparingLive"));
        preparing = true;
        bool prepared = false;
        try {
            if (config.UsesDualLayout()) {
                if (config.dualLayout.portrait.canvasUuid.empty() || config.dualLayout.landscape.canvasUuid.empty())
                    error = obs_module_text("Primary.ConfigureCanvases");
                else {
                    dual = std::make_unique<DualOutputSession>();
                    prepared = dual->Start(config, [this](const nlohmann::json& profiles) { SaveDevices(profiles); });
                    if (!prepared) error = dual->Error();
                    else {
                        obs_output_set_video_encoder(hold, dual->VideoEncoder(false));
                        obs_output_set_audio_encoder(hold, dual->AudioEncoder(false), 0);
                    }
                }
            } else prepared = StartSingle();
        } catch (const std::exception&) { error = "TikTok stream preparation failed."; }
        preparing = false;
        if (!prepared || cancelled) Stop();
        if (!pending) return;
        startup.start();
        timer = new QTimer(this);
        QObject::connect(timer, &QTimer::timeout, this, [this] { Poll(); }); timer->start(100);
    }

    bool StartSingle() {
        OBSDataAutoRelease outputSettings = obs_output_get_settings(hold);
        child = OBSOutputAutoRelease(obs_output_create("rtmp_output", "TikTok primary transport", outputSettings, nullptr)).Get();
        childService = OBSServiceAutoRelease(obs_service_create("rtmp_custom", "TikTok primary transport", nullptr, nullptr)).Get();
        if (!child || !childService) { error = "Could not create the TikTok transport."; return false; }
        auto video = obs_output_get_video_encoder(hold), audio = obs_output_get_audio_encoder(hold, 0);
        if (!video || !audio) { error = "Configure OBS video and audio encoders before streaming."; return false; }
        obs_output_set_video_encoder(child, video); obs_output_set_audio_encoder(child, audio, 0);
        obs_output_set_service(child, childService);
        obs_output_set_reconnect_settings(child, 5, 2);
        std::string server, key;
        if (config.connectionMethod == ConnectionMethod::TikTokLiveStudio) {
            native = std::make_unique<TikTokLiveSession>();
            if (!native->Start(config, child, [this](const nlohmann::json& profiles) { SaveDevices(profiles); })) { error = native->Error(); return false; }
            server = native->Server(); key = native->Key();
        } else {
            auto result = std::async(std::launch::async, [this] {
                TikTokRequestCancellation cancellation(&cancelled);
                const auto category = config.streamlabsCategoryId.has_value() ? *config.streamlabsCategoryId : StreamlabsAPI::CategorySearch(config.streamlabsAccessToken, config.streamlabsCategory);
                return StreamlabsAPI::StartStream(config.streamlabsAccessToken, config.streamlabsTitle.empty() ? config.name : config.streamlabsTitle,
                    category, config.streamlabsMatureContent ? 1 : 0);
            });
            QEventLoop loop; QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) loop.quit(); });
            poll.start(30); loop.exec();
            auto [success, message, rtmp, streamKey] = result.get();
            if (!success) { error = QString::fromStdString(message); return false; }
            server = rtmp; key = streamKey; streamlabsId = StreamlabsAPI::ExtractStreamId(key);
        }
        if (cancelled) return false;
        OBSDataAutoRelease settings = obs_service_get_settings(childService);
        obs_data_set_string(settings, "server", server.c_str()); obs_data_set_string(settings, "key", key.c_str());
        obs_service_update(childService, settings);
        auto handler = obs_output_get_signal_handler(child);
        signal_handler_connect(handler, "start", Started, this); signal_handler_connect(handler, "stop", Stopped, this);
        signal_handler_connect(handler, "reconnect", Reconnecting, this); signal_handler_connect(handler, "reconnect_success", Started, this);
        childRequested = true;
        if (!obs_output_start(child)) { childRequested = false; error = obs_module_text("Error.StartOutput"); return false; }
        return true;
    }

    void Stop() {
        cancelled = true; stopping = true;
        if (dual) dual->Stop();
        if (native && preparing) native->CancelPreparation();
        if (child && childRequested && !childStopped) obs_output_force_stop(child);
        if (!preparing && (!dual || !dual->Running()) && (!childRequested || childStopped)) Finish();
    }

    void Poll() {
        if (finishing || !pending) return;
        if (cancelled && !stopping) Stop();
        if (!pending) return;
        bool ready = false, ended = false;
        if (dual) {
            bytes = dual->TotalBytes(); dropped = dual->DroppedFrames();
            SetStatus(dual->Status()); ready = dual->Live(); ended = !dual->Running();
            if (ended && !dual->Error().isEmpty()) error = dual->Error();
        } else if (child) {
            bytes = obs_output_get_total_bytes(child); dropped = obs_output_get_frames_dropped(child);
            ready = childStarted && !childStopped && !childReconnecting; ended = childStopped || !childRequested;
            if (childStopped && childStopCode != OBS_OUTPUT_SUCCESS && error.isEmpty()) {
                const char* detail = obs_output_get_last_error(child);
                error = detail && *detail ? QString::fromUtf8(detail) : QStringLiteral("TikTok transport failed.");
            }
            if (native && !native->Error().isEmpty()) { error = native->Error(); Stop(); }
            SetStatus(obs_module_text(stopping ? "Status.Stopping" : ready ? "Status.Streaming" : childReconnecting ? "Status.Reconnecting" : "Status.Connecting"));
        }
        if (!stopping && ended) { if (error.isEmpty()) error = "TikTok output stopped."; Stop(); }
        if (!pending) return;
        if (captured && !stopping && reconnectReported == ready) {
            reconnectReported = !ready;
            calldata_t signal{};
            calldata_set_ptr(&signal, "output", hold.Get());
            calldata_set_int(&signal, "timeout_sec", 2);
            signal_handler_signal(obs_output_get_signal_handler(hold), ready ? "reconnect_success" : "reconnect", &signal);
            calldata_free(&signal);
        }
        if (!captured && ready && !stopping) {
            if (!obs_output_initialize_encoders(hold, 0) || !obs_output_begin_data_capture(hold, 0)) { error = "Could not activate the OBS TikTok output."; Stop(); }
            else { captured = true; if (native) native->ConfirmStarted(); }
        }
        if (!captured && !stopping && startup.elapsed() > 65000) { error = obs_module_text("Dual.StartTimeout"); Stop(); }
        if (stopping && ended) Finish();
    }

    void Finish() {
        if (finishing || preparing || !pending) return;
        finishing = true;
        if (timer) { timer->stop(); timer->deleteLater(); timer = nullptr; }
        if (child) {
            auto handler = obs_output_get_signal_handler(child);
            signal_handler_disconnect(handler, "start", Started, this); signal_handler_disconnect(handler, "stop", Stopped, this);
            signal_handler_disconnect(handler, "reconnect", Reconnecting, this); signal_handler_disconnect(handler, "reconnect_success", Started, this);
        }
        dual.reset();
        if (native) {
            native->Stop();
            const auto cleanupError = native->Error();
            if (!cleanupError.isEmpty() && !error.contains(cleanupError)) error += (error.isEmpty() ? "" : "\n") + cleanupError;
            native.reset();
        }
        if (!streamlabsId.empty()) {
            auto token = config.streamlabsAccessToken, id = std::exchange(streamlabsId, {});
            auto result = std::async(std::launch::async, [token, id] { return StreamlabsAPI::EndStream(token, id); });
            QEventLoop loop; QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) loop.quit(); });
            poll.start(30); loop.exec();
            try {
                if (!result.get()) error += (error.isEmpty() ? "" : "\n") + QStringLiteral("Could not confirm that Streamlabs ended the LIVE. Check TikTok.");
            } catch (...) { error += (error.isEmpty() ? "" : "\n") + QStringLiteral("Could not confirm that Streamlabs ended the LIVE. Check TikTok."); }
        }
        child = nullptr; childService = nullptr;
        SetStatus(error);
        OBSOutput output = hold; hold = nullptr;
        pending = false; captured = false;
        if (output) {
            if (!error.isEmpty()) obs_output_set_last_error(output, error.toUtf8().constData());
            obs_output_signal_stop(output, error.isEmpty() ? OBS_OUTPUT_SUCCESS : OBS_OUTPUT_ERROR);
        }
        finishing = false;
    }
};
}

void SetPrimaryServiceSaveCallback(std::function<void(obs_service_t*)> callback) { saveService = std::move(callback); }
QString PrimaryTikTokStatus() { std::lock_guard lock(statusMutex); return latestStatus; }
bool PrimaryTikTokBusy() {
    std::lock_guard lock(instancesMutex);
    for (auto output : instances) if (output->pending) return true;
    return false;
}
void StopPrimaryTikTokOutputs() {
    QList<QPointer<PrimaryOutput>> outputs;
    { std::lock_guard lock(instancesMutex); for (auto output : instances) outputs.push_back(output); }
    bool nestedPreparation = false;
    for (auto output : outputs) if (output && output->pending) {
        nestedPreparation |= output->preparing || output->finishing;
        output->cancelled = true; output->Stop();
    }
    // A cancellation received inside preparation's network event loop must
    // unwind that loop first. Otherwise drain stop signals before OBS tears
    // down the profile, encoders or graphics subsystem.
    if (!nestedPreparation) {
        QEventLoop loop; QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (!PrimaryTikTokBusy()) loop.quit(); });
        poll.start(20);
        if (PrimaryTikTokBusy()) loop.exec();
    }
}
void ReleasePrimaryTikTokObjects() {
    QList<QPointer<PrimaryOutput>> outputs;
    { std::lock_guard lock(instancesMutex); for (auto output : instances) outputs.push_back(output); }
    for (auto output : outputs) if (output) QCoreApplication::sendPostedEvents(output, QEvent::DeferredDelete);
}

void RegisterPrimaryTikTokOutput() {
    obs_service_info service{};
    service.id = TikTokPrimaryServiceId;
    service.get_name = [](void*) { return "TikTok"; };
    service.create = [](obs_data_t*, obs_service_t*) -> void* { return new char; };
    service.destroy = [](void* data) { delete static_cast<char*>(data); };
    service.get_protocol = [](void*) { return "RTMP"; };
    service.get_output_type = [](void*) { return TikTokPrimaryOutputId; };
    service.initialize = [](void*, obs_output_t* output) {
        if (!obs_output_get_delay(output)) return true;
        obs_output_set_last_error(output, "Disable OBS stream delay for managed TikTok streaming.");
        return false;
    };
    service.get_connect_info = [](void*, uint32_t type) { return type == OBS_SERVICE_CONNECT_INFO_SERVER_URL ? "TikTok" : ""; };
    service.get_supported_video_codecs = [](void*) -> const char** { static const char* codecs[]{"h264", "hevc", nullptr}; return codecs; };
    service.get_supported_audio_codecs = [](void*) -> const char** { static const char* codecs[]{"aac", nullptr}; return codecs; };
    obs_register_service(&service);
    obs_output_info output{};
    output.id = TikTokPrimaryOutputId; output.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
    output.get_name = [](void*) { return "TikTok primary output"; };
    output.protocols = "RTMP;RTMPS"; output.encoded_video_codecs = "h264;hevc"; output.encoded_audio_codecs = "aac";
    output.create = [](obs_data_t*, obs_output_t* handle) -> void* { return new PrimaryOutput(handle); };
    output.destroy = [](void* data) { static_cast<PrimaryOutput*>(data)->deleteLater(); };
    output.start = [](void* data) {
        auto state = static_cast<PrimaryOutput*>(data);
        if (obs_output_get_delay(state->raw)) { obs_output_set_last_error(state->raw, "Disable OBS stream delay for managed TikTok streaming."); return false; }
        if (state->pending.exchange(true)) return false;
        state->cancelled = false; state->hold = state->raw;
        QMetaObject::invokeMethod(state, [state] { state->Start(); }, Qt::QueuedConnection);
        return true;
    };
    output.stop = [](void* data, uint64_t) { auto state = static_cast<PrimaryOutput*>(data); state->cancelled = true;
        QMetaObject::invokeMethod(state, [state] { state->Stop(); }, Qt::QueuedConnection); };
    output.encoded_packet = [](void*, encoder_packet*) {};
    output.get_total_bytes = [](void* data) { return static_cast<PrimaryOutput*>(data)->bytes.load(); };
    output.get_dropped_frames = [](void* data) { return static_cast<PrimaryOutput*>(data)->dropped.load(); };
    obs_register_output(&output);
}
