#include "dual-output-session.h"
#include "dual-layout-widget.h"
#include "layout-scene.h"
#include "tiktok-live-session.h"
#include <obs.hpp>
#include <obs-module.h>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>
#include <array>
#include <cstring>

namespace {
struct OutputFeed {
    obs_output_t* output = nullptr;
    obs_service_t* service = nullptr;
    OBSEncoder video, audio;
    LayoutScene scene;
    std::atomic_bool started{false}, stopped{false}, reconnecting{false};
    std::atomic_int stopCode{0};
    bool startRequested = false;
    uint64_t lastBytes = 0;
    double kbps = 0;
    static void Started(void* p, calldata_t*) { auto& s = *static_cast<OutputFeed*>(p); s.started = true; s.reconnecting = false; }
    static void Stopped(void* p, calldata_t* data) { auto& s = *static_cast<OutputFeed*>(p); s.stopCode = static_cast<int>(calldata_int(data, "code")); s.stopped = true; }
    static void Reconnect(void* p, calldata_t*) { static_cast<OutputFeed*>(p)->reconnecting = true; }
    void Connect(bool enabled) {
        auto handler = obs_output_get_signal_handler(output);
        auto connect = enabled ? signal_handler_connect : signal_handler_disconnect;
        connect(handler, "start", Started, this); connect(handler, "stop", Stopped, this);
        connect(handler, "reconnect", Reconnect, this); connect(handler, "reconnect_success", Started, this);
    }
    ~OutputFeed() {
        if (output) { Connect(false); if (obs_output_active(output)) obs_output_force_stop(output); obs_output_release(output); }
        if (service) obs_service_release(service);
        video = nullptr; audio = nullptr;
    }
};

OBSEncoder MakeAudio(const LayoutAudioConfig& config, const std::string& name)
{
    const char* codec = obs_get_encoder_codec(config.encoderId.c_str());
    if (!codec || strcmp(codec, "aac") || config.mixer < 0 || config.mixer >= MAX_AUDIO_MIXES) return {};
    OBSDataAutoRelease settings = obs_encoder_defaults(config.encoderId.c_str());
    if (!settings) settings = obs_data_create();
    OBSDataAutoRelease saved = obs_data_create_from_json(config.encoderParams.is_object() ? config.encoderParams.dump().c_str() : "{}");
    obs_data_apply(settings, saved);
    obs_data_set_int(settings, "bitrate", config.bitrate);
    OBSEncoderAutoRelease encoder = obs_audio_encoder_create(config.encoderId.c_str(), name.c_str(), settings, static_cast<size_t>(config.mixer), nullptr);
    if (encoder) obs_encoder_set_audio(encoder, obs_get_audio());
    return encoder.Get();
}
}

struct DualOutputSession::State {
    std::array<std::unique_ptr<OutputFeed>, 2> feeds;
    std::unique_ptr<TikTokLiveSession> live;
    QTimer timer;
    QElapsedTimer startup, statistics;
    bool running = false, confirmed = false, stopping = false, finishing = false, preparing = false;
    QString error;
    int startupTimeoutMs = 60000;
};
DualOutputSession::DualOutputSession(QObject* parent, int startupTimeoutMs) : QObject(parent), state_(std::make_unique<State>())
{
    state_->startupTimeoutMs = startupTimeoutMs;
    QObject::connect(&state_->timer, &QTimer::timeout, this, [this] { Poll(); });
}
DualOutputSession::~DualOutputSession() { Finish(); }

bool DualOutputSession::Start(const OutputTargetConfig& config, std::function<void(const nlohmann::json&)> saveDevices)
{
    auto& s = *state_;
    if (s.running || s.live) return false;
    if (const auto error = ValidateDualLayout(config)) { s.error = obs_module_text(error); return false; }
    if (!config.UsesDualLayout()) { s.error = "Dual layout is not enabled."; return false; }
    s.error.clear();
    s.running = true;
    OBSEncoder shared;
    if (config.dualLayout.sameAudio) shared = MakeAudio(config.dualLayout.sharedAudio, config.id + "-dual-shared-audio");
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        const bool portrait = i == 0;
        const auto& cfg = portrait ? config.dualLayout.portrait : config.dualLayout.landscape;
        const auto name = config.id + (portrait ? "-portrait" : "-landscape");
        auto feed = std::make_unique<OutputFeed>();
        auto& f = *feed;
        s.feeds[i] = std::move(feed);
        if (!f.scene.Create(cfg, &s.error)) { Finish(); return false; }
        const auto codec = obs_get_encoder_codec(cfg.encoderId.c_str());
        if (!codec || (strcmp(codec, "h264") && strcmp(codec, "hevc"))) { s.error = obs_module_text("Dual.Error.Encoder"); Finish(); return false; }
        OBSDataAutoRelease settings = obs_encoder_defaults(cfg.encoderId.c_str());
        if (!settings) settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", cfg.bitrate);
        obs_data_set_int(settings, "keyint_sec", 2);
        obs_data_set_string(settings, "rate_control", "CBR");
        OBSDataAutoRelease saved = obs_data_create_from_json(cfg.encoderParams.is_object() ? cfg.encoderParams.dump().c_str() : "{}");
        obs_data_apply(settings, saved);
        obs_data_set_int(settings, "bitrate", cfg.bitrate);
        OBSEncoderAutoRelease video = obs_video_encoder_create(cfg.encoderId.c_str(), (name + "-video").c_str(), settings, nullptr);
        f.video = video.Get();
        f.audio = config.dualLayout.sameAudio ? shared : MakeAudio(LayoutAudio(config.dualLayout, portrait), name + "-audio");
        if (!f.video || !f.audio) { s.error = obs_module_text("Dual.Error.Encoder"); Finish(); return false; }
        obs_encoder_set_video(f.video, f.scene.Video());
        obs_encoder_set_frame_rate_divisor(f.video, static_cast<uint32_t>(cfg.fpsDivisor));
        OBSDataAutoRelease outputSettings = obs_data_create_from_json(config.outputParam.is_object() ? config.outputParam.dump().c_str() : "{}");
        f.output = obs_output_create("rtmp_output", name.c_str(), outputSettings, nullptr);
        OBSDataAutoRelease serviceSettings = obs_data_create();
        f.service = obs_service_create("rtmp_custom", (name + "-service").c_str(), serviceSettings, nullptr);
        if (!f.output || !f.service) { s.error = "Could not create both RTMP outputs."; Finish(); return false; }
        obs_output_set_service(f.output, f.service);
        obs_output_set_video_encoder(f.output, f.video);
        obs_output_set_audio_encoder(f.output, f.audio, 0);
        obs_output_set_reconnect_settings(f.output, 5, 2);
        f.Connect(true);
    }
    s.live = std::make_unique<TikTokLiveSession>();
    s.preparing = true;
    const bool prepared = s.live->Start(config, std::vector<obs_output*>{s.feeds[0]->output, s.feeds[1]->output}, std::move(saveDevices));
    s.preparing = false;
    if (!prepared || s.stopping) {
        s.error = s.live->Error(); Finish(); return false;
    }
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        auto& f = *s.feeds[i];
        OBSDataAutoRelease settings = obs_service_get_settings(f.service);
        obs_data_set_string(settings, "server", s.live->Server(i == 1).c_str());
        obs_data_set_string(settings, "key", s.live->Key(i == 1).c_str());
        obs_service_update(f.service, settings);
    }
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        auto& f = *s.feeds[i];
        f.startRequested = true;
        if (!obs_output_start(f.output)) {
            s.error = QString::fromUtf8(obs_module_text(i == 0 ? "Dual.Portrait" : "Dual.Landscape")) + ": " + obs_module_text("Error.StartOutput");
            Finish(); return false;
        }
    }
    s.startup.start(); s.statistics.start(); s.timer.start(250);
    return true;
}

void DualOutputSession::Poll()
{
    auto& s = *state_;
    if (!s.running || s.finishing) return;
    if (!s.live->Error().isEmpty()) { s.error = s.live->Error(); Stop(); }
    bool allStarted = true, allStopped = true;
    const auto elapsed = s.statistics.elapsed();
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        auto& f = *s.feeds[i];
        if (!f.scene.SourceExists() && !s.stopping) { s.error = obs_module_text("Dual.Error.SceneMissing"); Stop(); }
        allStarted = allStarted && f.started && !f.stopped;
        // A connecting output is not active yet. Only its stop signal proves
        // that an accepted start request has finished shutting down.
        allStopped = allStopped && (!f.startRequested || f.stopped);
        if (f.stopped && !s.stopping) {
            s.error = QString::fromUtf8(obs_module_text(i == 0 ? "Dual.Portrait" : "Dual.Landscape")) + ": " + obs_module_text("Dual.OutputStopped");
            const char* detail = obs_output_get_last_error(f.output);
            if (detail && *detail) s.error += " " + QString::fromUtf8(detail);
            Stop();
        }
        if (elapsed >= 1000) {
            const auto bytes = obs_output_get_total_bytes(f.output);
            f.kbps = bytes >= f.lastBytes ? static_cast<double>(bytes - f.lastBytes) * 8 / elapsed : 0;
            f.lastBytes = bytes;
        }
    }
    if (elapsed >= 1000) s.statistics.restart();
    if (!s.confirmed && allStarted && !s.stopping) { s.confirmed = true; s.live->ConfirmStarted(); }
    if (!s.confirmed && !s.stopping && s.startup.elapsed() > s.startupTimeoutMs) { s.error = obs_module_text("Dual.StartTimeout"); Stop(); }
    if (s.stopping && allStopped) Finish();
}
void DualOutputSession::Stop()
{
    auto& s = *state_;
    if (!s.running || s.stopping) return;
    s.stopping = true;
    if (s.preparing && s.live) s.live->CancelPreparation();
    for (const auto& f : s.feeds) if (f && f->output && f->startRequested && !f->stopped) obs_output_force_stop(f->output);
}
void DualOutputSession::Finish()
{
    auto& s = *state_;
    if (s.finishing) return;
    s.finishing = true; s.timer.stop();
    for (const auto& f : s.feeds) if (f && f->output && obs_output_active(f->output)) obs_output_force_stop(f->output);
    if (s.live) {
        s.live->Stop();
        if (!s.live->Error().isEmpty() && !s.error.contains(s.live->Error())) s.error += (s.error.isEmpty() ? "" : "\n") + s.live->Error();
        s.live.reset();
    }
    for (auto& f : s.feeds) f.reset();
    s.running = false; s.stopping = false; s.confirmed = false; s.finishing = false;
}
bool DualOutputSession::Running() const { return state_->running; }
bool DualOutputSession::Live() const {
    return state_->confirmed && !state_->stopping && state_->feeds[0] && state_->feeds[1] &&
        !state_->feeds[0]->reconnecting && !state_->feeds[1]->reconnecting;
}
QString DualOutputSession::Error() const { return state_->error; }
obs_encoder_t* DualOutputSession::VideoEncoder(bool portrait) const { const auto& feed = state_->feeds[portrait ? 0 : 1]; return feed ? feed->video.Get() : nullptr; }
obs_encoder_t* DualOutputSession::AudioEncoder(bool portrait) const { const auto& feed = state_->feeds[portrait ? 0 : 1]; return feed ? feed->audio.Get() : nullptr; }
uint64_t DualOutputSession::TotalBytes() const { uint64_t bytes = 0; for (const auto& feed : state_->feeds) if (feed && feed->output) bytes += obs_output_get_total_bytes(feed->output); return bytes; }
int DualOutputSession::DroppedFrames() const { int frames = 0; for (const auto& feed : state_->feeds) if (feed && feed->output) frames += obs_output_get_frames_dropped(feed->output); return frames; }
QString DualOutputSession::Status() const
{
    const auto& s = *state_;
    if (!s.running) return s.error;
    QStringList rows;
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        const auto& f = s.feeds[i];
        const char* status = s.stopping ? "Status.Stopping" : !f || !f->started ? "Status.Connecting" : f->reconnecting ? "Status.Reconnecting" : "Status.Streaming";
        rows << QString("%1: %2 — %3 kbps").arg(obs_module_text(i == 0 ? "Dual.Portrait" : "Dual.Landscape"), obs_module_text(status)).arg(f ? f->kbps : 0, 0, 'f', 0);
    }
    return rows.join('\n');
}
