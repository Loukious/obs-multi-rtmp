// Real libobs canvases, audio mixing, encoders, output signals, and packet
// callbacks. The tiny capture codecs/transport and all HTTP replies stay local.
#include "dual-output-session.h"
#include "dual-layout-widget.h"
#include "layout-scene.h"
#include "shared-scene-view.h"
#include "edit-widget.h"
#include "connection-settings.h"
#include "connection-login.h"
#include "account-info.h"
#include "primary-canvas.h"
#include "generated-layout.h"
#include "primary-output.h"
#include "primary-settings.h"
#include "streamlabs-api.h"
#include "tiktok-safety.h"
#include "tiktok-native/http_fixture.hpp"
#include "tiktok-native/request_cancellation.hpp"
#include <obs-module.h>
#include <obs.hpp>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QSpinBox>
#include <QScreen>
#include <QScrollArea>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <Windows.h>

static std::map<std::string, std::string> translations;
extern "C" const char* obs_module_text(const char* key) {
    const auto found = translations.find(key); return found == translations.end() ? key : found->second.c_str();
}
static std::optional<bool> accountAccess = true;
bool LoginTikTokWithQr(QWidget*, OutputTargetConfig&) { return false; }
QString DetectStreamlabsDesktopAccount(QWidget*) { return {}; }
AccountInfoResult LookupAccountInfo(const OutputTargetConfig&, const std::atomic_bool&) { return {"fixture_account", nullptr, {}, {}, {}, accountAccess}; }
StreamlabsCategoryResult SearchStreamlabsCategories(const std::string&, const QString&, const std::atomic_bool&) { return {}; }
TikTokGameResult LookupTikTokGames(const QString&, const std::atomic_bool&) { return {}; }
QString text(const char* key) { return key; }
QString translated_or(const char*, const QString& fallback) { return fallback; }
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void Until(const std::function<bool()>& condition, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) { QApplication::processEvents(); QThread::msleep(5); }
    Check(condition(), "OBS runtime condition timed out");
}
static QByteArray Json(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
static const char* Name(void*) { return "Local test"; }

struct CaptureStats {
    std::atomic_int videos{0}, audios{0}, peak{0}, center{0}, corner{0}, bottomRight{0};
    std::atomic_bool signedSize{false}, firstIndex{false};
    void Reset() { videos = 0; audios = 0; peak = 0; center = 0; corner = 0; signedSize = false; firstIndex = false; }
};
static std::array<CaptureStats, 2> stats;
static std::atomic_int outputCount{0}, encoderCount{0}, serviceCount{0};
static std::atomic_int creates{0}, finishes{0}, batches{0}, heartbeats{0};
static bool roomExists = false, incompatible = false, waitForCancel = false;
static int streamlabsStarts = 0, streamlabsEnds = 0;
static bool streamlabsReject = false;
static bool nativeReject = false;
static bool nativeSettingsConflict = false;
static QString lastCreateBody;
std::string StreamlabsAPI::ExtractStreamId(const std::string&) { return "fixture-stream"; }
std::string StreamlabsAPI::CategorySearch(const std::string&, const std::string&) { return "fixture-category"; }
std::tuple<bool, std::string, std::string, std::string> StreamlabsAPI::StartStream(const std::string&, const std::string&, const std::string&, const int) {
    ++streamlabsStarts;
    if (streamlabsReject) return {false, StreamlabsApiError({{"message", "An unexpected error occurred when trying to connect to the server"},
        {"data", QJsonObject{{"message", "user already has a live room in TikTok Platform"}}}}).toStdString(), {}, {}};
    return {true, {}, "rtmp://fixture/live", "fixture-key"};
}
bool StreamlabsAPI::EndStream(const std::string&, const std::string&) { ++streamlabsEnds; return true; }
enum class Mode { Normal, RejectLandscape, WaitLandscape, FailLandscape };
static Mode mode = Mode::Normal;

struct Encoder {
    obs_encoder_t* encoder;
    std::array<uint8_t, 9> bytes{0, 0, 0, 1, 0x65, 0x88, 0x99, 0x55, 0x11};
};
static void RegisterCaptureCodecs() {
    obs_encoder_info video{};
    video.id = "test_h264"; video.type = OBS_ENCODER_VIDEO; video.codec = "h264"; video.get_name = Name;
    video.create = [](obs_data_t*, obs_encoder_t* e) -> void* { ++encoderCount; return new Encoder{e}; };
    video.destroy = [](void* p) { --encoderCount; delete static_cast<Encoder*>(p); };
    video.get_properties = [](void*) {
        auto props = obs_properties_create();
        obs_properties_add_int(props, "bitrate", "Bitrate", 100, 50000, 100);
        obs_properties_add_int(props, "keyint_sec", "Keyframe interval", 1, 10, 1);
        auto preset = obs_properties_add_list(props, "preset", "Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(preset, "Fast", "fast"); obs_property_list_add_string(preset, "Quality", "quality");
        return props;
    };
    video.get_video_info = [](void*, video_scale_info* info) { info->format = VIDEO_FORMAT_I420; };
    video.encode = [](void* p, encoder_frame* frame, encoder_packet* packet, bool* received) {
        auto& e = *static_cast<Encoder*>(p);
        const auto w = obs_encoder_get_width(e.encoder), h = obs_encoder_get_height(e.encoder);
        auto& s = stats[w < h ? 0 : 1];
        s.center = frame->data[0][h/2 * frame->linesize[0] + w/2]; s.corner = frame->data[0][0];
        s.bottomRight = frame->data[0][(h - 10) * frame->linesize[0] + w - 10];
        packet->data = e.bytes.data(); packet->size = e.bytes.size();
        packet->pts = packet->dts = frame->pts; packet->type = OBS_ENCODER_VIDEO; packet->keyframe = true;
        *received = true; return true;
    };
    obs_register_encoder(&video);
    obs_encoder_info audio{};
    audio.id = "test_aac"; audio.type = OBS_ENCODER_AUDIO; audio.codec = "aac"; audio.get_name = Name;
    audio.create = video.create; audio.destroy = video.destroy;
    audio.get_frame_size = [](void*) -> size_t { return 1024; };
    audio.get_audio_info = [](void*, audio_convert_info* info) { info->format = AUDIO_FORMAT_FLOAT_PLANAR; };
    audio.encode = [](void* p, encoder_frame* frame, encoder_packet* packet, bool* received) {
        auto& e = *static_cast<Encoder*>(p);
        const auto samples = reinterpret_cast<const float*>(frame->data[0]);
        float peak = 0; for (uint32_t i = 0; i < frame->frames; ++i) peak = std::max(peak, std::abs(samples[i]));
        e.bytes[0] = static_cast<uint8_t>(std::min(255.0f, peak * 255));
        packet->data = e.bytes.data(); packet->size = 1; packet->pts = packet->dts = frame->pts;
        packet->type = OBS_ENCODER_AUDIO; *received = true; return true;
    };
    obs_register_encoder(&audio);
}

struct Transport : QObject {
    obs_output_t* output;
    int layout;
    explicit Transport(obs_output_t* o) : output(o), layout(QString(obs_output_get_name(o)).contains("portrait") ? 0 : 1) { ++outputCount; }
    ~Transport() { --outputCount; }
};
static void RegisterTransport() {
    obs_output_info output{}; output.id = "rtmp_output";
    output.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
    output.get_name = Name; output.protocols = "RTMP"; output.encoded_video_codecs = "h264"; output.encoded_audio_codecs = "aac";
    output.create = [](obs_data_t*, obs_output_t* o) -> void* { return new Transport(o); };
    output.destroy = [](void* p) { delete static_cast<Transport*>(p); };
    output.start = [](void* p) {
        auto& t = *static_cast<Transport*>(p);
        if (t.layout == 1 && mode == Mode::RejectLandscape) return false;
        if (t.layout == 1 && mode == Mode::WaitLandscape) return true;
        if (!obs_output_can_begin_data_capture(t.output, 0) || !obs_output_initialize_encoders(t.output, 0)) return false;
        const bool ok = obs_output_begin_data_capture(t.output, 0);
        if (ok && t.layout == 1 && mode == Mode::FailLandscape)
            QTimer::singleShot(350, &t, [&t] { obs_output_set_last_error(t.output, "fixture terminal failure"); obs_output_signal_stop(t.output, OBS_OUTPUT_ERROR); });
        return ok;
    };
    output.stop = [](void* p, uint64_t) { obs_output_signal_stop(static_cast<Transport*>(p)->output, OBS_OUTPUT_SUCCESS); };
    output.encoded_packet = [](void* p, encoder_packet* packet) {
        if (!packet) return;
        const auto& t = *static_cast<Transport*>(p); auto& s = stats[t.layout];
        if (packet->type == OBS_ENCODER_AUDIO) { ++s.audios; if (packet->size) s.peak = std::max(s.peak.load(), int(packet->data[0])); }
        else {
            ++s.videos;
            const auto data = QByteArray::fromRawData(reinterpret_cast<const char*>(packet->data), static_cast<qsizetype>(packet->size));
            if (data.contains(t.layout == 0 ? "\"push_video_height\":320,\"push_video_width\":180" : "\"push_video_height\":180,\"push_video_width\":320")) s.signedSize = true;
            if (data.contains("\"sei_index\":0,")) s.firstIndex = true;
        }
    };
    output.get_total_bytes = [](void* p) -> uint64_t {
        const auto& s = stats[static_cast<Transport*>(p)->layout]; return s.videos * 1000 + s.audios * 100;
    };
    obs_register_output(&output);
    obs_service_info service{}; service.id = "rtmp_custom"; service.get_name = Name;
    service.create = [](obs_data_t*, obs_service_t* s) -> void* { ++serviceCount; return s; };
    service.destroy = [](void*) { --serviceCount; };
    service.get_protocol = [](void*) { return "RTMP"; };
    obs_register_service(&service);
}

struct Color { vec4 color; };
static void RegisterSources() {
    obs_source_info source{}; source.id = "test_color"; source.type = OBS_SOURCE_TYPE_INPUT;
    source.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW; source.get_name = Name;
    source.create = [](obs_data_t* settings, obs_source_t*) -> void* {
        auto c = new Color; vec4_from_rgba(&c->color, static_cast<uint32_t>(obs_data_get_int(settings, "color"))); return c;
    };
    source.destroy = [](void* p) { delete static_cast<Color*>(p); };
    source.get_width = [](void*) -> uint32_t { return 320; }; source.get_height = [](void*) -> uint32_t { return 180; };
    source.video_render = [](void* p, gs_effect_t*) {
        auto effect = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &static_cast<Color*>(p)->color);
        while (gs_effect_loop(effect, "Solid")) gs_draw_sprite(nullptr, 0, 320, 180);
    };
    obs_register_source(&source);
    obs_source_info audio{}; audio.id = "test_audio"; audio.type = OBS_SOURCE_TYPE_INPUT;
    audio.output_flags = OBS_SOURCE_AUDIO; audio.get_name = Name;
    audio.create = [](obs_data_t*, obs_source_t* s) -> void* { return s; }; audio.destroy = [](void*) {};
    obs_register_source(&audio);
}

static void InstallHttpFixture() {
    tiktok_http_fixture = [](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray& body) -> TikTokHttpFixtureResponse {
        const auto path = url.path();
        if (path == "/get_domains/v4/") return {200, "{}"};
        if (path == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"gb"}})"};
        if (path == "/signatures") return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a","x-gorgon":"g"})"};
        if (path == "/webcast/eco/violation_list/") return {200, R"({"status_code":0,"data":{"active_count":0,"records":[]}})"};
        if (path == "/webcast/game/basic/create_info/") return {200, R"({"status_code":0,"data":{"allow_multi_stream":true}})"};
        if (path == "/webcast/anchor_tool/threshold/check/") return {200, R"({"status_code":0,"data":{"allowed":true}})"};
        QJsonObject room{{"id_str", "9001"}, {"stream_id_str", "9002"}, {"owner_user_id_str", "9003"},
            {"stream_url", QJsonObject{{"rtmp_push_url", "rtmp://fixture/live/portrait"}}}};
        if (!incompatible) room["multi_stream_url"] = QJsonObject{{"rtmp_push_url", "rtmp://fixture/live/landscape"}};
        if (nativeSettingsConflict) room["room_auth"] = QJsonObject{{"Gift", 1}, {"Chat", 1}};
        if (path == "/webcast/room/continue/") return {200, Json({{"data", roomExists ? QJsonObject{{"room", room}} : QJsonObject{}}})};
        if (path == "/webcast/room/create/") {
            ++creates;
            lastCreateBody = QString::fromUtf8(body);
            if (nativeReject) return {200, R"({"status_code":4003019,"message":"Request failed","data":{"prompts":"LIVE restricted for this account"}})"};
            roomExists = true; return {200, Json({{"data", room}})};
        }
        if (path == "/webcast/room/ping/anchor/") {
            const auto status = QUrlQuery(QString::fromUtf8(body)).queryItemValue("status");
            if (status == "4") { ++finishes; roomExists = false; }
            if (status == "2") ++heartbeats;
            return {200, "{\"status_code\":0}"};
        }
        if (path == "/webcast/room/anchor_pre_finish/") return {200, "{\"status_code\":0}"};
        if (path == "/framesign/batch") {
            ++batches;
            if (waitForCancel) {
                // Frame signer receives cancellation explicitly, so let the
                // fixture return after the UI has requested cancellation.
                QThread::msleep(250);
            }
            QJsonArray values;
            const auto now = QDateTime::currentSecsSinceEpoch();
            for (qint64 i = now - 1; i < now + 300; ++i)
                values.append(QJsonObject{{"timestamp", i}, {"signinfo", "fixture"}, {"signvalue", "fixture"}, {"signversion", "1.0"}});
            return {200, QJsonDocument(values).toJson(QJsonDocument::Compact)};
        }
        throw std::runtime_error("Unexpected runtime fixture HTTP request");
    };
}

static void AssertReleased() {
    Until([] { return outputCount == 0 && encoderCount == 0 && serviceCount == 0; });
    Check(!obs_get_encoder_by_name("runtime-portrait-video"), "Portrait encoder released");
    int ephemeral = 0;
    obs_enum_canvases([](void* p, obs_canvas_t* canvas) { if (obs_canvas_get_flags(canvas) & EPHEMERAL) ++*static_cast<int*>(p); return true; }, &ephemeral);
    Check(ephemeral == 0, "Private layout canvases released after stop");
}

static void EditorFixture(const QString& cookiePath, obs_scene_t* portrait, obs_scene_t* landscape, const QString& screenshots)
{
    if (!screenshots.isEmpty()) {
        QSettings locale(QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath("../data/locale/en-US.ini"), QSettings::IniFormat);
        for (const auto& key : locale.allKeys()) translations[key.toStdString()] = locale.value(key).toString().toStdString();
    }
    auto target = std::make_shared<OutputTargetConfig>();
    target->id = "editor-fixture"; target->name = "TikTok fixture";
    target->connectionMethod = ConnectionMethod::TikTokLiveStudio; target->rapidApiKey = "fixture";
    target->cookieFilePath = cookiePath.toStdString(); target->tiktokTopicId = "6";
    target->dualLayout.enabled = true;
    target->dualLayout.portrait.sceneUuid = obs_source_get_uuid(obs_scene_get_source(portrait));
    target->dualLayout.landscape.sceneUuid = obs_source_get_uuid(obs_scene_get_source(landscape));
    for (auto v : {&target->dualLayout.portrait, &target->dualLayout.landscape}) { v->encoderId = "test_h264"; v->audio.encoderId = "test_aac"; }
    target->dualLayout.sharedAudio.encoderId = "test_aac";
    GlobalMultiOutputConfig().targets.push_back(target);
    std::unique_ptr<EditOutputWidget> editor(createEditOutputWidget(target->id, nullptr));
    editor->show();
    auto mode = editor->findChild<QComboBox*>("tiktokLayout");
    Until([&] { return mode->currentIndex() == 1; });
    auto tabs = editor->findChild<QTabWidget*>();
    Check(tabs && tabs->count() == 5, "Service, Video, Audio, Output and Other tabs persist in dual mode");
    auto capture = [&](const char* name) {
        QElapsedTimer settle; settle.start();
        while (settle.elapsed() < 120) { QApplication::processEvents(); QThread::msleep(5); }
        auto scroll = editor->findChild<QScrollArea*>("outputEditorScroll");
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots);
            editor->grab().save(screenshots + '/' + name + ".png");
        }
        Check(editor->findChild<QPushButton*>("saveOutputSettings")->isVisible(), "Save button remains outside scrolling content");
    };
    capture("editor-service");
    auto portraitScene = editor->findChild<QComboBox*>("portraitScene");
    const int selectedPortrait = portraitScene->currentIndex();
    portraitScene->setCurrentIndex(0);
    editor->findChild<QPushButton*>("saveOutputSettings")->click();
    Check(editor->isVisible() && tabs->currentIndex() == 1 && editor->focusWidget() == portraitScene &&
        editor->findChild<QLabel*>("dualVideoError")->isVisible(), "Dock save reveals the missing dual scene in Video instead of the cookie input");
    portraitScene->setCurrentIndex(selectedPortrait);
    tabs->setCurrentIndex(1); QApplication::processEvents();
    Check(editor->findChild<QWidget*>("dualVideoSettings")->isVisible() && !editor->findChild<QWidget*>("singleVideoSettings")->isVisible(), "Video tab shows the active dual controls");
    const auto left = editor->findChild<QWidget*>("portraitVideoSettings"), right = editor->findChild<QWidget*>("landscapeVideoSettings");
    Check(left->y() == right->y() && left->x() < right->x(), "Full editor keeps layout columns side by side");
    capture("editor-video");
    bool advancedOpened = false;
    QTimer::singleShot(100, [&] {
        for (auto top : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QDialog*>(top); dialog && top != editor.get() && dialog->isVisible()) {
            advancedOpened = true; dialog->accept();
        }
    });
    editor->findChild<QPushButton*>("portraitVideoAdvanced")->click();
    Check(advancedOpened, "Per-layout encoder properties open and save through the actual OBS properties editor");
    tabs->setCurrentIndex(2); QApplication::processEvents();
    Check(editor->findChild<QWidget*>("sharedAudio")->isVisible(), "Audio tab retains common audio controls");
    capture("editor-audio-shared");
    editor->findChild<QCheckBox*>("dualSameAudio")->setChecked(false);
    Check(editor->findChild<QComboBox*>("portraitAudioTrack")->isVisible() && editor->findChild<QComboBox*>("landscapeAudioTrack")->isVisible(), "Audio tab reveals independent track selectors");
    capture("editor-audio-independent");
    if (editor->screen()->availableGeometry().width() >= 1250) {
        editor->resize(1180, 780); capture("editor-audio-expanded");
        auto scroll = editor->findChild<QScrollArea*>("outputEditorScroll");
        Check(scroll->widget()->width() == scroll->viewport()->width(), "Editor contents expand with the resized viewport");
        const auto expandedWidth = editor->findChild<QComboBox*>("portraitAudioEncoder")->width();
        const auto chosenSize = editor->size();
        editor->findChild<QPushButton*>("refreshTikTokAccount")->click();
        Until([&] { return editor->findChild<QLineEdit*>("canDualLayout")->text() == obs_module_text("Account.Yes"); });
        mode->setCurrentIndex(1); tabs->setCurrentIndex(1); tabs->setCurrentIndex(2); capture("editor-audio-refreshed");
        Check(editor->size() == chosenSize, "Tab changes and asynchronous account results preserve user size");
        editor->resize(900, 640); capture("editor-audio-narrow");
        Check(editor->findChild<QComboBox*>("portraitAudioEncoder")->width() < expandedWidth, "Audio dropdowns resize with the dialog");
    }
    tabs->setCurrentIndex(4); QApplication::processEvents();
    Check(editor->findChild<QWidget*>("otherSettingsPage")->isVisible(), "Other settings stay accessible in dual mode"); capture("editor-other");
    mode->setCurrentIndex(0); tabs->setCurrentIndex(1); QApplication::processEvents();
    Check(editor->findChild<QWidget*>("singleVideoSettings")->isVisible(), "Single mode restores original video controls in the same tab");
    tabs->setCurrentIndex(2); capture("editor-audio-single");
    Check(editor->findChild<QWidget*>("singleAudioSettings")->isVisible() &&
        editor->findChild<QComboBox*>("singleAudioEncoder")->width() > editor->width() / 3,
        "Single-stream audio dropdown also uses available width");
    mode->setCurrentIndex(1);
    accountAccess = false; editor->findChild<QPushButton*>("refreshTikTokAccount")->click();
    Until([&] { return editor->findChild<QLineEdit*>("canDualLayout")->text() == obs_module_text("Account.No"); });
    Check(mode->currentIndex() == 0 && !(mode->model()->flags(mode->model()->index(1, 0)) & Qt::ItemIsEnabled), "Account denial disables dual in the actual editor");
    tabs->setCurrentIndex(0); capture("editor-access-locked");
    editor->findChild<QComboBox*>("tiktokClient")->setCurrentIndex(1); capture("editor-mobile-automatic");
    editor.reset(); GlobalMultiOutputConfig().targets.clear(); accountAccess = true;
    AssertReleased();
    translations.clear();
}

static void RunFixtures(const QString& cookiePath, obs_scene_t* portrait, obs_scene_t* landscape) {
    OutputTargetConfig config;
    config.id = "runtime"; config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.rapidApiKey = "fixture"; config.cookieFilePath = cookiePath.toStdString();
    config.tiktokLiveSettings = {false, false, "third_party"};
    config.tiktokDeviceProfiles = {{"pc", {{"device_id", "11"}, {"install_id", "12"}}}};
    config.dualLayout.enabled = true;
    config.dualLayout.portrait.sceneUuid = obs_source_get_uuid(obs_scene_get_source(portrait));
    config.dualLayout.landscape.sceneUuid = obs_source_get_uuid(obs_scene_get_source(landscape));
    config.dualLayout.portrait.width = 180; config.dualLayout.portrait.height = 320;
    config.dualLayout.landscape.width = 320; config.dualLayout.landscape.height = 180;
    config.dualLayout.sharedAudio.encoderId = "test_aac";
    for (auto v : {&config.dualLayout.portrait, &config.dualLayout.landscape}) {
        v->encoderId = "test_h264"; v->audio.encoderId = "test_aac";
        v->encoderParams = {{"preset", "quality"}};
        v->audio.encoderParams = {{"fixture-quality", 7}};
    }
    config.dualLayout.portrait.fpsDivisor = 2;
    for (int audioMode = 0; audioMode < 4; ++audioMode) {
        for (auto& s : stats) s.Reset();
        config.dualLayout.sameAudio = audioMode == 2;
        config.dualLayout.sharedAudio.mixer = 4;
        config.dualLayout.portrait.audio.mixer = audioMode == 1 ? 2 : 4;
        config.dualLayout.landscape.audio.mixer = audioMode == 1 || audioMode == 3 ? 4 : 2;
        config.dualLayout.portrait.crop = audioMode == 1;
        const int before = finishes, beforeBatch = batches;
        DualOutputSession session;
        Check(session.Start(config, {}), "Paired startup succeeds with primary OBS output stopped");
        Check(QUrlQuery(lastCreateBody).queryItemValue("gift_auth") == "2" && QUrlQuery(lastCreateBody).queryItemValue("chat_auth") == "2" &&
            QUrlQuery(lastCreateBody).queryItemValue("ecom_bc_toggle") == "5", "Dock dual startup passes configured gifts, comments and disclosure to room creation");
        Until([&] { return session.Live() && stats[0].audios > 20 && stats[1].audios > 20 && stats[0].signedSize && stats[1].signedSize; });
        Check(stats[0].firstIndex && stats[1].firstIndex && batches == beforeBatch + 1, "Both signed feeds start at their own index zero with one shared batch request");
        OBSEncoderAutoRelease p = obs_get_encoder_by_name(audioMode == 2 ? "runtime-dual-shared-audio" : "runtime-portrait-audio");
        OBSEncoderAutoRelease l = obs_get_encoder_by_name(audioMode == 2 ? "runtime-dual-shared-audio" : "runtime-landscape-audio");
        Check(p && l && (audioMode == 2 ? p.Get() == l.Get() : p.Get() != l.Get()), "Shared encoder identity only when sharing is enabled");
        Check(obs_encoder_get_mixer_index(p) == size_t(LayoutAudio(config.dualLayout, true).mixer) &&
            obs_encoder_get_mixer_index(l) == size_t(LayoutAudio(config.dualLayout, false).mixer), "User's track indices reach real OBS audio encoders");
        {
            OBSEncoderAutoRelease video = obs_get_encoder_by_name("runtime-portrait-video");
            OBSDataAutoRelease settings = obs_encoder_get_settings(video);
            Check(std::string(obs_data_get_string(settings, "preset")) == "quality" && obs_encoder_get_frame_rate_divisor(video) == 2,
                "Advanced video preset and per-layout frame rate reach the actual encoder");
            if (audioMode != 2) {
                OBSDataAutoRelease audio = obs_encoder_get_settings(p);
                Check(obs_data_get_int(audio, "fixture-quality") == 7, "Advanced audio settings reach the actual encoder");
            }
        }
        for (size_t i = 0; i < 2; ++i) {
            const int expected = LayoutAudio(config.dualLayout, i == 0).mixer == 4 ? 51 : 153;
            Check(std::abs(stats[i].peak.load() - expected) <= 2, "Selected track amplitude reaches only its assigned layout, without doubling");
        }
        std::cout << "Samples: portrait=" << stats[0].center << "/" << stats[0].corner << ", landscape=" << stats[1].center << "/" << stats[1].corner << '\n';
        Check(stats[0].center > 50 && stats[1].center > stats[0].center + 25, "Both selected scenes render distinct colors");
        Check(audioMode == 1 ? stats[0].corner > 60 : stats[0].corner < 25, "Portrait Crop fills the frame; Fit letterboxes without stretching");
        if (audioMode == 0) {
            auto duplicateConfig = config; duplicateConfig.id = "duplicate";
            DualOutputSession duplicate;
            Check(!duplicate.Start(duplicateConfig, {}) && duplicate.Error().contains("already"), "Duplicate account startup rejected while pair remains live");
            Check(session.Live() && finishes == before, "Rejected duplicate must not finish the existing room");
            OBSOutputAutoRelease output = obs_get_output_by_name("runtime-landscape");
            auto handler = obs_output_get_signal_handler(output);
            calldata_t data{}; signal_handler_signal(handler, "reconnect", &data);
            Check(!session.Live(), "One reconnecting feed makes pair non-live");
            signal_handler_signal(handler, "reconnect_success", &data);
            Check(session.Live(), "Pair recovers after feed reconnects");
            Until([] { return heartbeats == 1; }, 12000);
            Check(batches == beforeBatch + 1, "One heartbeat and signer cache serve both feeds");
        }
        p = nullptr; l = nullptr;
        session.Stop(); session.Stop(); Until([&] { return !session.Running(); });
        Check(finishes == before + 2, "Exactly one room finish sequence despite duplicate Stop");
        AssertReleased();
    }
    for (auto failureMode : {Mode::RejectLandscape, Mode::WaitLandscape, Mode::FailLandscape}) {
        mode = failureMode; const int before = finishes;
        DualOutputSession session(nullptr, 700);
        const bool started = session.Start(config, {});
        Check(started == (mode != Mode::RejectLandscape), "OBS start result distinguished from eventual readiness");
        Until([&] { return !session.Running(); });
        Check(!session.Error().isEmpty() && finishes == before + 2, "Partial failure or timeout stops both and ends owned room once");
        AssertReleased();
    }
    mode = Mode::Normal;
    {
        roomExists = true; incompatible = true; const int before = finishes;
        DualOutputSession session;
        Check(!session.Start(config, {}) && finishes == before && roomExists, "Incompatible pre-existing room is not ended");
        roomExists = false; incompatible = false; AssertReleased();
    }
    {
        waitForCancel = true; const int before = finishes;
        QTimer cancel;
        QObject::connect(&cancel, &QTimer::timeout, [] {
            for (auto window : QApplication::topLevelWidgets())
                if (auto button = window->findChild<QPushButton*>("cancelTikTokPreparation"); button && button->isEnabled()) button->click();
        });
        cancel.start(50);
        DualOutputSession session;
        Check(!session.Start(config, {}) && session.Error().contains("Cancelled"), "Preparation cancellation prevents outputs starting");
        Check(finishes == before + 2, "Cancellation cleans up the newly created room");
        cancel.stop(); waitForCancel = false; AssertReleased();
    }
    {
        waitForCancel = true; const int before = finishes;
        DualOutputSession session;
        QTimer::singleShot(50, &session, [&] { session.Stop(); });
        Check(!session.Start(config, {}) && !session.Running(), "Stop hotkey during preparation cannot start the pair later");
        Check(finishes == before + 2, "Stop during preparation ends the owned room once");
        waitForCancel = false; AssertReleased();
    }
    {
        DualOutputSession session; Check(session.Start(config, {}), "Account lease released after cancellation");
        Until([&] { return session.Live(); });
        obs_source_remove(obs_scene_get_source(portrait));
        Until([&] { return !session.Running(); });
        Check(session.Error().contains("SceneMissing"), "Scene deletion stops both outputs with a visible error");
        AssertReleased();
    }
}

class NativeSettingsFixture : public QDialog {
    Q_OBJECT
public:
    QDialogButtonBox* buttons;
    bool refuse = false;
    int saves = 0;
    NativeSettingsFixture() {
        auto layout = new QVBoxLayout(this);
        auto page = new QWidget(this); page->setObjectName("streamPage");
        auto fields = new QVBoxLayout(page);
        auto service = new QComboBox(page); service->setObjectName("service"); service->addItem("Custom", 2); service->addItem("Fixture service"); fields->addWidget(service);
        for (const char* name : {"customServer", "key"}) { auto edit = new QLineEdit(page); edit->setObjectName(name); fields->addWidget(edit); }
        layout->addWidget(page);
        buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->setObjectName("buttonBox"); layout->addWidget(buttons);
        // OBS uses connectSlotsByName, which creates a meta-method connection.
        connect(buttons, SIGNAL(clicked(QAbstractButton*)), this, SLOT(on_buttonBox_clicked(QAbstractButton*)));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        resize(1000, 740);
    }
public slots:
    void on_buttonBox_clicked(QAbstractButton* button) {
        const auto role = buttons->buttonRole(button);
        if (role == QDialogButtonBox::ApplyRole || role == QDialogButtonBox::AcceptRole) {
            if (refuse) return;
            ++saves; buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        }
        if (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::RejectRole) close();
    }
};

static void PrimaryFixtures(const QString& cookiePath, obs_scene_t* portrait, obs_scene_t* landscape, const QString& screenshots) {
    std::cout << "Primary fixtures begin\n";
    {
        OBSServiceAutoRelease service = obs_service_create(TikTokPrimaryServiceId, "delay fixture", nullptr, nullptr);
        OBSOutputAutoRelease output = obs_output_create(TikTokPrimaryOutputId, "delay fixture", nullptr, nullptr);
        obs_output_set_service(output, service); obs_output_set_delay(output, 3, 0);
        Check(!obs_output_start(output) && !PrimaryTikTokBusy() && !obs_output_active(output), "Delay is rejected before OBS begins capture");
    }
    OutputTargetConfig config;
    config.id = "tiktok-primary"; config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.tiktokLiveSettings = {false, false, "both"};
    config.tiktokTopicId = "6";
    config.rapidApiKey = "fixture"; config.cookieFilePath = cookiePath.toStdString();
    config.tiktokDeviceProfiles = {{"pc", {{"device_id", "11"}, {"install_id", "12"}}}};
    config.dualLayout.enabled = true; config.dualLayout.sameAudio = false;
    config.dualLayout.portrait.sceneUuid = obs_source_get_uuid(obs_scene_get_source(portrait));
    config.dualLayout.landscape.sceneUuid = obs_source_get_uuid(obs_scene_get_source(landscape));
    config.dualLayout.portrait.width = 180; config.dualLayout.portrait.height = 320;
    config.dualLayout.landscape.width = 320; config.dualLayout.landscape.height = 180;
    config.dualLayout.portrait.audio.mixer = 4; config.dualLayout.landscape.audio.mixer = 2;
    config.dualLayout.sharedAudio.encoderId = "test_aac";
    for (auto v : {&config.dualLayout.portrait, &config.dualLayout.landscape}) { v->encoderId = "test_h264"; v->audio.encoderId = "test_aac"; }
    std::vector<OBSCanvas> canvases;
    auto create = [&](const char* name, obs_video_info* info, int flags) {
        auto canvas = obs_canvas_create(name, info, flags); canvases.emplace_back(canvas); return canvas;
    };
    auto remove = [&](obs_canvas_t* canvas) {
        obs_canvas_remove(canvas);
        std::erase_if(canvases, [canvas](const OBSCanvas& value) { return value == canvas; });
    };
    QString error;
    {
        auto failed = config; int count = 0;
        Check(!PreparePrimaryCanvases(failed, &error, [&](const char* name, obs_video_info* info, int flags) -> obs_canvas_t* {
            return ++count == 2 ? nullptr : create(name, info, flags);
        }, remove), "Second canvas creation failure rolls back the first");
        Check(canvases.empty() && failed.dualLayout.portrait.canvasUuid.empty(), "Failed preparation does not change configuration");
    }
    Check(PreparePrimaryCanvases(config, &error, create, remove), "Create persistent native canvases");
    Check(canvases.size() == 2 && !(obs_canvas_get_flags(canvases[0]) & EPHEMERAL), "Two persistent canvases created");
    auto checkSceneSelectors = [&](const OutputTargetConfig& prepared) {
        DualLayoutWidget editor;
        editor.SetAccess(true); editor.SetNativeCanvasMode(true); editor.Load(prepared.dualLayout);
        auto portraitChoice = editor.findChild<QComboBox*>("portraitScene");
        auto landscapeChoice = editor.findChild<QComboBox*>("landscapeScene");
        Check(portraitChoice->currentData() == obs_source_get_uuid(obs_scene_get_source(portrait)) &&
            landscapeChoice->currentData() == obs_source_get_uuid(obs_scene_get_source(landscape)),
            "Prepared and reloaded primary layouts display the original input scenes");
        for (auto combo : {portraitChoice, landscapeChoice}) {
            Check(combo->findData(QString::fromStdString(prepared.dualLayout.portrait.sceneUuid)) < 0 &&
                combo->findData(QString::fromStdString(prepared.dualLayout.landscape.sceneUuid)) < 0,
                "Generated portrait and landscape compositions are absent from the scene choices");
        }
        auto saved = prepared; editor.Save(saved.dualLayout);
        Check(saved.dualLayout.portrait.canvasUuid == prepared.dualLayout.portrait.canvasUuid &&
            saved.dualLayout.portrait.sceneUuid == prepared.dualLayout.portrait.sceneUuid &&
            saved.dualLayout.landscape.sceneUuid == prepared.dualLayout.landscape.sceneUuid && editor.Validate(saved),
            "Saving source selections retains prepared outputs without nesting or duplicating canvases");
    };
    checkSceneSelectors(config);
    for (const auto* layout : {&config.dualLayout.portrait, &config.dualLayout.landscape}) {
        OBSSourceAutoRelease source = obs_get_source_by_uuid(layout->sceneUuid.c_str());
        OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
        obs_data_set_bool(metadata, GeneratedLayoutKey, false);
    }
    checkSceneSelectors(config); // Existing installations predate the explicit marker.
    for (const auto* layout : {&config.dualLayout.portrait, &config.dualLayout.landscape}) {
        OBSSourceAutoRelease source = obs_get_source_by_uuid(layout->sceneUuid.c_str());
        OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
        obs_data_set_bool(metadata, GeneratedLayoutKey, true);
    }
    Check(PreparePrimaryCanvases(config, &error, create, remove) && canvases.size() == 2, "Apply reuses bound canvases");
    Check(DecodePrimaryConfig(EncodePrimaryConfig(config).c_str()).dualLayout.portrait.canvasUuid == config.dualLayout.portrait.canvasUuid, "Primary canvas binding persists");
    auto invalid = config; invalid.dualLayout.portrait.width += 2;
    Check(!PreparePrimaryCanvases(invalid, &error, create, remove), "Existing canvas cannot silently change resolution");

    // OBS persists canvas identity and scenes, but obs_load_canvas does not
    // restore a video mix. Exercise the same serialization as an OBS restart.
    auto reloadLayout = [&](LayoutVideoConfig& layout) {
        OBSCanvasAutoRelease originalCanvas = obs_get_canvas_by_uuid(layout.canvasUuid.c_str());
        OBSSourceAutoRelease originalScene = obs_get_source_by_uuid(layout.sceneUuid.c_str());
        OBSDataAutoRelease canvasData = obs_save_canvas(originalCanvas);
        OBSDataAutoRelease sceneData = obs_save_source(originalScene);
        layout.canvasUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        layout.sceneUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        obs_data_set_string(canvasData, "uuid", layout.canvasUuid.c_str());
        obs_data_set_string(sceneData, "uuid", layout.sceneUuid.c_str());
        obs_data_set_string(sceneData, "canvas_uuid", layout.canvasUuid.c_str());
        OBSCanvasAutoRelease restoredCanvas = obs_load_canvas(canvasData);
        Check(restoredCanvas && !obs_canvas_has_video(restoredCanvas), "OBS reload reproduces an existing canvas without video");
        canvases.emplace_back(restoredCanvas.Get());
        OBSDataArrayAutoRelease scenes = obs_data_array_create(); obs_data_array_push_back(scenes, sceneData);
        obs_load_sources(scenes, nullptr, nullptr);
        OBSSourceAutoRelease restoredScene = obs_get_source_by_uuid(layout.sceneUuid.c_str());
        Check(restoredScene && obs_scene_find_source(obs_scene_from_source(restoredScene),
            obs_source_get_name(obs_scene_get_source(layout.width < layout.height ? portrait : landscape))),
            "Reloaded layout retains the original source scene inside its saved composition");
    };
    for (bool throughSettings : {true, false}) {
        auto restored = config;
        reloadLayout(restored.dualLayout.portrait); reloadLayout(restored.dualLayout.landscape);
        checkSceneSelectors(restored);
        const auto saved = EncodePrimaryConfig(restored);
        if (throughSettings) {
            const auto count = canvases.size();
            Check(PreparePrimaryCanvases(restored, &error, create, remove) && canvases.size() == count,
                "Apply restores saved canvas video instead of requiring another scene or creating new canvases");
        } else {
            for (const auto* layout : {&restored.dualLayout.portrait, &restored.dualLayout.landscape}) {
                OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(layout->canvasUuid.c_str());
                OBSSceneAutoRelease other = obs_canvas_scene_create(canvas, "Different current scene");
                obs_canvas_set_channel(canvas, 0, obs_scene_get_source(other));
                for (int opened = 0; opened < 2; ++opened) {
                    LayoutScene preview;
                    Check(preview.CreatePreview(*layout, &error) && preview.Video(), "Open and reopen the actual preview canvas after OBS reload");
                    // Read the same Render() used by the preview's native
                    // display, without depending on desktop visibility.
                    QElapsedTimer settle; settle.start(); Until([&] { return settle.elapsed() > 150; });
                    struct Pixels { int red = 0, green = 0, corner = 0; } pixels;
                    obs_enter_graphics();
                    auto target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
                    auto surface = gs_stagesurface_create(layout->width, layout->height, GS_RGBA);
                    bool captured = false;
                    gs_projection_push();
                    if (target && surface && gs_texrender_begin(target, layout->width, layout->height)) {
                        vec4 black{}; gs_clear(GS_CLEAR_COLOR, &black, 0, 0);
                        gs_ortho(0, float(layout->width), 0, float(layout->height), -100, 100);
                        preview.Render(); gs_texrender_end(target);
                        gs_stage_texture(surface, gs_texrender_get_texture(target));
                        uint8_t* bytes = nullptr; uint32_t stride = 0;
                        if (gs_stagesurface_map(surface, &bytes, &stride)) {
                            auto center = bytes + layout->height / 2 * stride + layout->width / 2 * 4;
                            pixels = {center[0], center[1], bytes[0]}; captured = true;
                            gs_stagesurface_unmap(surface);
                        }
                    }
                    gs_projection_pop();
                    gs_stagesurface_destroy(surface); gs_texrender_destroy(target);
                    obs_leave_graphics();
                    Check(captured, "Read actual preview pixels");
                    const bool portraitLayout = layout->width < layout->height;
                    std::cout << "Preview pixels after reload: " << pixels.red << ',' << pixels.green << '\n';
                    Check(portraitLayout ? pixels.red > 200 && pixels.green < 30 : pixels.green > 200 && pixels.red < 30,
                        "Saved preview renders the selected scene's color instead of black or the canvas's current scene");
                    if (portraitLayout) Check(pixels.corner < 30, "Saved portrait preview retains Fit framing");
                    OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
                    Check(current == obs_scene_get_source(other), "Preview never switches the persistent canvas's current scene");
                }
                obs_canvas_set_channel(canvas, 0, nullptr);
                obs_source_remove(obs_scene_get_source(other));
            }
        }
        Check(EncodePrimaryConfig(restored) == saved, "Restoring video preserves both saved scene selections and all layout settings");
        if (!throughSettings) {
            // Leave one saved canvas cold to retain startup-restoration coverage.
            reloadLayout(restored.dualLayout.landscape);
            config = restored;
        }
    }

    for (auto failure : {Mode::Normal, Mode::WaitLandscape, Mode::RejectLandscape, Mode::FailLandscape}) {
        mode = failure; for (auto& s : stats) s.Reset(); const int before = finishes;
        {
            OBSDataAutoRelease settings = obs_data_create(); obs_data_set_string(settings, "tiktok_config", EncodePrimaryConfig(config).c_str());
            OBSServiceAutoRelease service = obs_service_create(TikTokPrimaryServiceId, "primary fixture", settings, nullptr);
            OBSOutputAutoRelease output = obs_output_create(TikTokPrimaryOutputId, "primary fixture", nullptr, nullptr);
            obs_output_set_service(output, service);
            Check(obs_output_start(output), "Primary accepts native OBS start");
            Check(PrimaryTikTokBusy() && !obs_output_active(output), "Preparation is pending before OBS becomes live");
            if (failure == Mode::WaitLandscape) {
                QTimer::singleShot(400, [&] {
                    Check(!obs_output_active(output), "One ready feed must not announce OBS live");
                    obs_output_force_stop(output);
                });
            }
            if (failure == Mode::Normal) {
                Until([&] { return !PrimaryTikTokBusy() || (obs_output_active(output) && stats[0].audios > 20 && stats[1].audios > 20); });
                if (!PrimaryTikTokBusy()) throw std::runtime_error(PrimaryTikTokStatus().toStdString());
                Check(stats[0].signedSize && stats[1].signedSize && stats[0].firstIndex && stats[1].firstIndex, "Primary sends two independently signed layouts");
                Check(encoderCount == 4, "Primary status adapter adds no encoders");
                Check(std::abs(stats[0].peak - 51) <= 2 && std::abs(stats[1].peak - 153) <= 2, "Primary honors arbitrary per-layout tracks without doubling");
                Check(obs_output_get_total_bytes(output) > 0, "OBS primary stats include transported bytes");
                int reconnects = 0, recovered = 0;
                auto increment = [](void* p, calldata_t*) { ++*static_cast<int*>(p); };
                auto handler = obs_output_get_signal_handler(output);
                signal_handler_connect(handler, "reconnect", increment, &reconnects);
                signal_handler_connect(handler, "reconnect_success", increment, &recovered);
                OBSOutputAutoRelease feed = obs_get_output_by_name("tiktok-primary-landscape");
                calldata_t signal{};
                signal_handler_signal(obs_output_get_signal_handler(feed), "reconnect", &signal);
                Until([&] { return reconnects == 1; });
                signal_handler_signal(obs_output_get_signal_handler(feed), "reconnect_success", &signal);
                Until([&] { return recovered == 1; });
                signal_handler_disconnect(handler, "reconnect", increment, &reconnects);
                signal_handler_disconnect(handler, "reconnect_success", increment, &recovered);
                obs_output_stop(output);
            }
            Until([] { return !PrimaryTikTokBusy(); });
            Until([&] { return !obs_output_active(output); });
            Check(finishes == before + 2, "Primary failure/stop closes one shared room");
        }
        AssertReleased();
        for (auto canvas : canvases) Check(!obs_canvas_removed(canvas), "Native canvases survive streaming stop");
    }
    mode = Mode::Normal;
    for (int variant = 0; variant < 8; ++variant) {
        auto single = config; single.dualLayout.enabled = false;
        streamlabsReject = variant == 4;
        nativeReject = variant == 5 || variant == 6;
        nativeSettingsConflict = variant == 7;
        if (variant == 6) {
            single.tiktokClient = "mobile";
            single.tiktokDeviceProfiles["mobile"] = {{"device_id", "991"}, {"install_id", "992"}, {"broadcast_type", "video"}};
        }
        if (variant == 1 || streamlabsReject) { single.connectionMethod = ConnectionMethod::Streamlabs; single.streamlabsAccessToken = "fixture"; }
        const int before = finishes;
        {
            OBSDataAutoRelease settings = obs_data_create(); obs_data_set_string(settings, "tiktok_config", EncodePrimaryConfig(single).c_str());
            OBSServiceAutoRelease service = obs_service_create(TikTokPrimaryServiceId, "primary fixture", settings, nullptr);
            OBSOutputAutoRelease output = obs_output_create(TikTokPrimaryOutputId, "primary fixture", nullptr, nullptr);
            OBSEncoderAutoRelease video = obs_video_encoder_create("test_h264", "OBS selected video", nullptr, nullptr);
            OBSEncoderAutoRelease audio = obs_audio_encoder_create("test_aac", "OBS selected audio", nullptr, 2, nullptr);
            obs_encoder_set_video(video, obs_get_video()); obs_encoder_set_audio(audio, obs_get_audio());
            obs_output_set_video_encoder(output, video); obs_output_set_audio_encoder(output, audio, 0); obs_output_set_service(output, service);
            for (auto& s : stats) s.Reset();
            waitForCancel = variant == 2;
            if (waitForCancel) QTimer::singleShot(50, [] { StopPrimaryTikTokOutputs(); });
            Check(obs_output_start(output), "Single primary start accepted");
            if (variant != 2 && !streamlabsReject && !nativeReject && !nativeSettingsConflict) {
                Until([&] { return obs_output_active(output) && stats[1].audios > 20; });
                Check(encoderCount == 2 && obs_output_get_video_encoder(output) == video.Get(), "Single primary uses OBS encoders without duplicates");
                if (variant == 0) {
                    Check(stats[1].signedSize, "Single primary signs the actual native dimensions");
                    Check(QUrlQuery(lastCreateBody).queryItemValue("commercial_content_promote_myself") == "true" &&
                        QUrlQuery(lastCreateBody).queryItemValue("commercial_content_promote_third_party") == "true", "Primary OBS passes saved content disclosure to room creation");
                }
                StopPrimaryTikTokOutputs();
                Check(!PrimaryTikTokBusy(), "Shutdown drains primary output cleanup");
            }
            Until([] { return !PrimaryTikTokBusy(); });
            waitForCancel = false;
            if (streamlabsReject) {
                Check(QString::fromUtf8(obs_output_get_last_error(output)) == "user already has a live room in TikTok Platform", "Actual OBS output retains the specific API reason for its error dialog");
                Check(stats[1].audios == 0 && streamlabsStarts == 2 && streamlabsEnds == 1 && finishes == before, "Rejected Streamlabs start never transmits or ends an existing LIVE");
            } else if (nativeSettingsConflict) {
                Check(QString::fromUtf8(obs_output_get_last_error(output)).contains("gifts and comments"), "OBS displays the server's contradiction of the disabled startup options");
                Check(stats[1].videos == 0 && stats[1].audios == 0 && finishes == before + 2 && !roomExists, "Conflicting new room is ended before any stream packets are sent");
            } else if (nativeReject) {
                Check(QString::fromUtf8(obs_output_get_last_error(output)).contains("LIVE restricted for this account"), "Actual OBS output retains the native desktop/mobile API rejection");
                Check(stats[1].audios == 0 && finishes == before && !roomExists, "Rejected native start never transmits or sends room cleanup");
            } else Check(variant == 1 ? streamlabsStarts == 1 && streamlabsEnds == 1 : finishes == before + 2, "Single primary cancellation/stop finishes its owned room");
        }
        AssertReleased();
    }
    streamlabsReject = false;
    nativeReject = false;
    nativeSettingsConflict = false;
    {
        if (!screenshots.isEmpty()) {
            QSettings locale(QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath("../data/locale/en-US.ini"), QSettings::IniFormat);
            for (const auto& key : locale.allKeys()) translations[key.toStdString()] = locale.value(key).toString().toStdString();
        }
        NativeSettingsFixture dialog;
        int committed = 0, prepared = 0, rolledBack = 0; bool enabled = true;
        auto saved = config;
        PrimarySettingsHost host;
        host.load = [&] { return saved; }; host.enabled = [&] { return enabled; }; host.busy = [] { return false; };
        host.prepare = [&](OutputTargetConfig& next, QString* reason) { ++prepared; return PreparePrimaryCanvases(next, reason, create, remove); };
        host.rollbackPrepare = [&](const auto&, const auto&) { ++rolledBack; };
        host.commit = [&](const auto& next, bool active) { ++committed; saved = next; enabled = active; };
        host.previousService = {{"type", "rtmp_custom"}, {"settings", {{"server", "rtmp://previous/live"}, {"key", "fixture-key"}}}};
        Check(AttachPrimaryTikTokSettings(&dialog, host), "Attach to native OBS Settings widget contract");
        dialog.show();
        Until([&] { return dialog.findChild<QComboBox*>("tiktokLayout")->currentIndex() == 1; });
        auto tabs = dialog.findChild<QTabWidget*>("primaryTikTokTabs");
        tabs->setCurrentIndex(1);
        auto bitrate = dialog.findChild<QSpinBox*>("portraitVideoBitrate"); bitrate->setValue(bitrate->value() + 100);
        auto apply = dialog.buttons->button(QDialogButtonBox::Apply);
        Check(apply->isEnabled(), "Video edits enable native Apply after reparenting");
        dialog.refuse = true; apply->click();
        Check(committed == 0 && rolledBack == 1 && dialog.isVisible(), "Native validation refusal prevents TikTok commit and close");
        dialog.refuse = false; apply->click();
        Check(committed == 1 && enabled && saved.dualLayout.portrait.bitrate == bitrate->value(), "Apply saves TikTok configuration with native settings");
        Check(dialog.findChild<QComboBox*>("portraitScene")->currentData() == obs_source_get_uuid(obs_scene_get_source(portrait)) &&
            dialog.findChild<QComboBox*>("landscapeScene")->currentData() == obs_source_get_uuid(obs_scene_get_source(landscape)),
            "Native Settings Apply keeps the user's source scene in both selectors");
        tabs->setCurrentIndex(2);
        auto track = dialog.findChild<QComboBox*>("portraitAudioTrack"); track->setCurrentIndex(1); track->activated(1);
        Check(apply->isEnabled(), "Audio edits enable native Apply"); apply->click();
        Check(saved.dualLayout.portrait.audio.mixer == 1, "Primary audio selection saved");
        auto cookieInput = dialog.findChild<QLineEdit*>("cookieFilePath");
        const auto alternateCookie = cookiePath + ".browse.json";
        Check(QFile::copy(cookiePath, alternateCookie), "Copy cookie fixture for Browse selection");
        cookieInput->setText(alternateCookie);
        Check(apply->isEnabled(), "Programmatic cookie Browse selection enables native Apply");
        apply->click();
        Check(saved.cookieFilePath == alternateCookie.toStdString() && !apply->isEnabled(), "Browse-selected cookie saves and clears dirty state");
        dialog.findChild<QCheckBox*>("tiktokAllowGifts")->setChecked(true);
        auto disclosure = dialog.findChild<QComboBox*>("tiktokContentDisclosure"); disclosure->setCurrentIndex(disclosure->findData("own_brand"));
        Check(apply->isEnabled(), "Room option changes enable native Apply"); apply->click();
        Check(saved.tiktokLiveSettings.allowGifts && !saved.tiktokLiveSettings.allowComments && saved.tiktokLiveSettings.contentDisclosure == "own_brand",
            "Native Apply persists gifts, comments and disclosure to primary settings");
        // Changing the cookie account resets access; wait for that lookup and
        // select dual mode before exercising dual validation.
        Until([&] { return dialog.findChild<QLineEdit*>("canDualLayout")->text() == obs_module_text("Account.Yes"); });
        dialog.findChild<QComboBox*>("tiktokLayout")->setCurrentIndex(1);
        const int beforeInvalid = committed, preparesBeforeInvalid = prepared;
        auto portraitScene = dialog.findChild<QComboBox*>("portraitScene");
        auto landscapeScene = dialog.findChild<QComboBox*>("landscapeScene");
        const int portraitIndex = portraitScene->currentIndex(), landscapeIndex = landscapeScene->currentIndex();
        portraitScene->setCurrentIndex(0); landscapeScene->setCurrentIndex(0);
        tabs->setCurrentIndex(0); cookieInput->setFocus();
        dialog.buttons->button(QDialogButtonBox::Ok)->click();
        Check(dialog.isVisible() && committed == beforeInvalid && prepared == preparesBeforeInvalid && tabs->currentIndex() == 1 &&
            dialog.focusWidget() == portraitScene && dialog.findChild<QLabel*>("dualVideoError")->isVisible() &&
            !dialog.findChild<QLabel*>("connectionError")->isVisible(), "Missing portrait scene opens Video and does not blame valid cookies or commit settings");
        if (!screenshots.isEmpty()) { QApplication::processEvents(); dialog.grab().save(screenshots + "/primary-missing-scene.png"); }
        portraitScene->setCurrentIndex(portraitIndex); tabs->setCurrentIndex(0);
        dialog.buttons->button(QDialogButtonBox::Ok)->click();
        Check(tabs->currentIndex() == 1 && dialog.focusWidget() == landscapeScene, "After choosing Portrait, validation points to the missing Landscape scene");
        landscapeScene->setCurrentIndex(landscapeIndex);
        auto audioEncoder = dialog.findChild<QComboBox*>("portraitAudioEncoder");
        const int audioIndex = audioEncoder->currentIndex(); audioEncoder->setCurrentIndex(-1);
        tabs->setCurrentIndex(0); dialog.buttons->button(QDialogButtonBox::Ok)->click();
        Check(tabs->currentIndex() == 2 && dialog.focusWidget() == audioEncoder && dialog.findChild<QLabel*>("dualAudioError")->isVisible(),
            "Audio validation opens Audio and focuses the invalid encoder");
        audioEncoder->setCurrentIndex(audioIndex); apply->setEnabled(true); apply->click();
        Check(committed == beforeInvalid + 1 && prepared == preparesBeforeInvalid + 1 && saved.dualLayout.enabled &&
            !dialog.findChild<QLabel*>("dualVideoError")->isVisible() && !dialog.findChild<QLabel*>("dualAudioError")->isVisible(),
            "Correcting layout fields saves dual mode with the same cookie file and clears errors");
        if (!screenshots.isEmpty()) {
            for (int i = 0; i < 3; ++i) { tabs->setCurrentIndex(i); QApplication::processEvents(); dialog.grab().save(screenshots + "/primary-settings-" + QString::number(i) + ".png"); }
        }
        auto provider = dialog.findChild<QComboBox*>("primaryStreamingProvider"); provider->setCurrentIndex(0);
        Check(dialog.findChild<QLineEdit*>("customServer")->text() == "rtmp://previous/live", "Switching back restores prior native server");
        apply->click(); Check(!enabled, "Switching away disables managed service");
        const int beforeCancel = committed, beforePrepare = prepared;
        provider->setCurrentIndex(1); dialog.buttons->button(QDialogButtonBox::Cancel)->click();
        Check(committed == beforeCancel && prepared == beforePrepare && !dialog.isVisible(), "Cancel neither prepares canvases nor changes service");
    }
    translations.clear();
    {
        DualLayoutWidget editor;
        editor.SetAccess(true); editor.SetNativeCanvasMode(true); editor.Load(config.dualLayout);
        auto choice = editor.findChild<QComboBox*>("portraitScene");
        choice->setCurrentIndex(choice->findData(QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(landscape)))));
        auto changed = config; editor.Save(changed.dualLayout);
        Check(changed.dualLayout.portrait.canvasUuid.empty() && changed.dualLayout.portrait.sceneUuid == obs_source_get_uuid(obs_scene_get_source(landscape)),
            "Changing the source scene releases only that layout's prepared binding");
        const auto count = canvases.size();
        Check(PreparePrimaryCanvases(changed, &error, create, remove) && canvases.size() == count + 1, "Changed layout creates one replacement composition");
        OBSCanvasAutoRelease old = obs_get_canvas_by_uuid(config.dualLayout.portrait.canvasUuid.c_str());
        {
            OBSSceneAutoRelease userScene = obs_canvas_scene_create(old, "User scene on generated canvas");
            RemoveReplacedPrimaryCanvases(config, changed, remove);
            Check(!obs_canvas_removed(old) && canvases.size() == count + 1, "Replacing a generated layout preserves canvases containing user-added scenes");
            obs_source_remove(obs_scene_get_source(userScene));
        }
        RemoveReplacedPrimaryCanvases(config, changed, remove);
        Check(obs_canvas_removed(old) && canvases.size() == count, "Committing an edited scene retires its old generated canvas without accumulating outputs");
        OBSSourceAutoRelease kept = obs_get_source_by_uuid(changed.dualLayout.portrait.sceneUuid.c_str());
        OBSSourceAutoRelease input = GeneratedLayoutInput(kept);
        Check(input == obs_scene_get_source(landscape), "Replacement uses the selected source directly, without nested generated scenes");
    }
    for (auto canvas : canvases) obs_canvas_remove(canvas);
    canvases.clear(); obs_wait_for_destroy_queue();
    std::cout << "PASS: primary native canvases, OBS lifecycle, single/dual transport, Apply/Cancel and provider switching\n";
}

static void SingleSceneFixture() {
    obs_output_info capture{}; capture.id = "scene_view_capture";
    capture.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
    capture.get_name = Name; capture.encoded_video_codecs = "h264";
    capture.create = [](obs_data_t*, obs_output_t* output) -> void* { return output; };
    capture.destroy = [](void*) {};
    capture.start = [](void* p) {
        auto output = static_cast<obs_output_t*>(p);
        return obs_output_initialize_encoders(output, 0) && obs_output_begin_data_capture(output, 0);
    };
    capture.stop = [](void* p, uint64_t) { obs_output_signal_stop(static_cast<obs_output_t*>(p), OBS_OUTPUT_SUCCESS); };
    capture.encoded_packet = [](void*, encoder_packet* packet) { if (packet) ++stats[0].videos; };
    obs_register_output(&capture);
    // Issue #4: a custom-sized Aitum scene must not be rendered through the
    // landscape main canvas before scaling to the requested portrait output.
    obs_video_info info{}; Check(obs_get_video_info(&info), "Main video info");
    info.base_width = info.output_width = 180;
    info.base_height = info.output_height = 320;
    OBSCanvasAutoRelease canvas = obs_canvas_create_private("Portrait fixture", &info, 0);
    Check(canvas != nullptr, "Portrait canvas creation");
    for (bool legacy : {true, false}) {
        OBSSceneAutoRelease scene = legacy ? obs_scene_create_private("Aitum-style scene")
            : obs_canvas_scene_create(canvas, "Native portrait scene");
        auto source = obs_scene_get_source(scene);
        if (legacy) {
            OBSDataAutoRelease settings = obs_source_get_settings(source);
            obs_data_set_bool(settings, "custom_size", true);
            obs_data_set_int(settings, "cx", 180); obs_data_set_int(settings, "cy", 320);
            obs_source_load2(source);
        }
        Check(obs_source_get_width(source) == 180 && obs_source_get_height(source) == 320,
            "Scene exposes its portrait dimensions");
        OBSDataAutoRelease settings = obs_data_create(); obs_data_set_int(settings, "color", 0xffffffff);
        OBSSourceAutoRelease white = obs_source_create_private("test_color", "Full-frame white", settings);
        auto item = obs_scene_add(scene, white);
        vec2 scale{180.f / 320, 320.f / 180}; obs_sceneitem_set_scale(item, &scale);
        OBSEncoderAutoRelease encoder = obs_video_encoder_create("test_h264", "Single portrait", nullptr, nullptr);
        obs_encoder_set_scaled_size(encoder, 90, 160);
        auto view = GetSharedSceneViewManager().Acquire(encoder, source);
        Check(bool(view), "Single output scene view");
        auto shared = GetSharedSceneViewManager().Acquire(encoder, source);
        Check(shared == view, "Outputs sharing an encoder retain the same scene view");
        view.reset();
        Check(obs_encoder_get_width(encoder) == 90 && obs_encoder_get_height(encoder) == 160,
            "Explicit encoder resolution is preserved");
        OBSOutputAutoRelease output = obs_output_create("scene_view_capture", "Portrait pixels", nullptr, nullptr);
        obs_output_set_video_encoder(output, encoder);
        stats[0].Reset();
        Check(obs_output_start(output), "Start single scene encoder");
        Until([] { return stats[0].videos >= 5; });
        OBSSceneAutoRelease other = obs_scene_create_private("Landscape switch");
        Check(!GetSharedSceneViewManager().Acquire(encoder, obs_scene_get_source(other)),
            "Cannot resize a scene view used by a live shared encoder");
        Check(GetSharedSceneViewManager().Acquire(encoder, source) == shared,
            "Live shared encoder keeps its original portrait view");
        obs_output_force_stop(output);
        Until([&] { return !obs_output_active(output); });
        Check(stats[0].corner > 220 && stats[0].center > 220 && stats[0].bottomRight > 220,
            "Portrait scene fills the frame without clipping or blank columns");
        Check(video_output_get_width(shared->Video()) == 180 && video_output_get_height(shared->Video()) == 320,
            "Single view uses scene dimensions, not the main canvas");
        auto switched = GetSharedSceneViewManager().Acquire(encoder, obs_scene_get_source(other));
        Check(switched == shared && video_output_get_width(switched->Video()) == 320
            && video_output_get_height(switched->Video()) == 180,
            "Stopped shared encoder rebuilds its view when scene dimensions change");
        switched.reset();
        shared.reset();
        if (!legacy) obs_canvas_scene_remove(scene);
    }
    obs_canvas_remove(canvas);
    std::cout << "PASS: single-output portrait pixels, native/custom scene sizes and shared view lifetime\n";
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        Check(argc > 1, "OBS data directory argument required");
        // Standalone harness has no OBS application data-directory layout.
#pragma warning(suppress: 4996)
        obs_add_data_path((QByteArray(argv[1]) + '/').constData());
        Check(obs_startup("en-US", nullptr, nullptr), "OBS startup");
        wchar_t modulePath[32768]{};
        Check(GetModuleFileNameW(GetModuleHandleW(L"obs.dll"), modulePath, 32768) != 0, "Find loaded OBS runtime");
        const auto graphics = (QFileInfo(QString::fromWCharArray(modulePath)).absolutePath() + "/libobs-d3d11.dll").toUtf8();
        obs_video_info video{}; video.graphics_module = graphics.constData();
        video.fps_num = 30; video.fps_den = 1;
        video.base_width = video.output_width = 320; video.base_height = video.output_height = 180;
        video.output_format = VIDEO_FORMAT_NV12; video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_PARTIAL;
        video.gpu_conversion = true;
        video.scale_type = OBS_SCALE_BICUBIC;
        Check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "D3D11 video initialization");
        obs_audio_info audio{48000, SPEAKERS_STEREO}; Check(obs_reset_audio(&audio), "Audio initialization");
        RegisterSources(); RegisterCaptureCodecs(); RegisterTransport(); RegisterPrimaryTikTokOutput(); InstallHttpFixture();
        SingleSceneFixture();
        {
            OBSSceneAutoRelease portrait = obs_scene_create("Portrait selection");
            OBSSceneAutoRelease landscape = obs_scene_create("Landscape selection");
            OBSDataAutoRelease settings = obs_data_create(); obs_data_set_int(settings, "color", 0xff0000ff);
            OBSSourceAutoRelease red = obs_source_create("test_color", "Red", settings, nullptr); obs_scene_add(portrait, red);
            obs_data_set_int(settings, "color", 0xff00ff00);
            OBSSourceAutoRelease green = obs_source_create("test_color", "Green", settings, nullptr); obs_scene_add(landscape, green);
            obs_set_output_source(0, obs_scene_get_source(landscape));
            OBSSourceAutoRelease track5 = obs_source_create("test_audio", "Track 5 signal", nullptr, nullptr);
            OBSSourceAutoRelease track3 = obs_source_create("test_audio", "Track 3 signal", nullptr, nullptr);
            obs_source_set_audio_mixers(track5, 1 << 4); obs_source_set_audio_mixers(track3, 1 << 2);
            obs_set_output_source(1, track5); obs_set_output_source(2, track3);
            std::jthread samples([&](std::stop_token stop) {
                std::array<float, 1024> low, high; low.fill(.2f); high.fill(.6f);
                auto next = os_gettime_ns();
                while (!stop.stop_requested()) {
                    obs_source_audio chunk{}; chunk.frames = 1024; chunk.samples_per_sec = 48000; chunk.speakers = SPEAKERS_STEREO;
                    chunk.format = AUDIO_FORMAT_FLOAT_PLANAR; chunk.timestamp = next;
                    chunk.data[0] = chunk.data[1] = reinterpret_cast<const uint8_t*>(low.data()); obs_source_output_audio(track5, &chunk);
                    chunk.data[0] = chunk.data[1] = reinterpret_cast<const uint8_t*>(high.data()); obs_source_output_audio(track3, &chunk);
                    next += 1024ULL * 1000000000 / 48000; os_sleepto_ns(next);
                }
            });
            QTemporaryDir temp; QFile cookie(temp.filePath("cookies.json")); Check(cookie.open(QIODevice::WriteOnly), "Cookie fixture");
            cookie.write(R"([{"domain":".tiktok.com","name":"sessionid","value":"runtime-dummy"}])"); cookie.close();
            // Localized screenshots are optional; assertions also support raw
            // translation keys when CTest runs without the locale argument.
            EditorFixture(cookie.fileName(), portrait, landscape, argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
            if (argc > 2) {
                const auto directory = QString::fromLocal8Bit(argv[2]); QDir().mkpath(directory);
                for (int layout = 0; layout < 3; ++layout) {
                    LayoutVideoConfig preview;
                    preview.sceneUuid = obs_source_get_uuid(obs_scene_get_source(layout == 2 ? landscape.Get() : portrait.Get()));
                    preview.width = layout == 2 ? 320 : 180; preview.height = layout == 2 ? 180 : 320; preview.crop = layout == 1;
                    QTimer::singleShot(1200, [&] {
                        for (auto window : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QDialog*>(window); dialog && dialog->isVisible()) {
                            // Desktop captures cannot verify an obscured preview
                            // and must not capture the user's foreground app.
                            if (GetForegroundWindow() != GetAncestor(reinterpret_cast<HWND>(dialog->winId()), GA_ROOT)) {
                                std::cout << "SKIP: native preview screenshot is obscured by another window.\n";
                                dialog->accept(); continue;
                            }
                            // Capture the composited desktop: window-DC captures
                            // on Windows omit a child's D3D swap chain.
                            const auto origin = dialog->mapToGlobal(QPoint(0, 0)) - dialog->screen()->geometry().topLeft();
                            const auto capture = dialog->screen()->grabWindow(0, origin.x(), origin.y(), dialog->width(), dialog->height());
                            Check(capture.save(directory + "/preview-" + QString::number(layout) + ".png"), "Save real native layout preview");
                            const auto pixel = capture.toImage().pixelColor(capture.width()/2, capture.height()/2);
                            Check(layout == 2 ? pixel.green() > 200 && pixel.red() < 30 : pixel.red() > 200 && pixel.green() < 30,
                                "Native preview displays the selected scene at its center");
                            dialog->accept();
                        }
                    });
                    ShowLayoutPreview(preview, nullptr);
                }
            }
            PrimaryFixtures(cookie.fileName(), portrait, landscape, argc > 2 ? QString::fromUtf8(argv[2]) : QString());
            RunFixtures(cookie.fileName(), portrait, landscape);
            samples.request_stop(); samples.join();
            for (int i = 0; i < 3; ++i) obs_set_output_source(i, nullptr);
            // The main canvas owns scene references in OBS 32.2. Remove our
            // test scenes explicitly before OBS tears down its source table.
            obs_canvas_scene_remove(portrait); obs_canvas_scene_remove(landscape);
        }
        obs_wait_for_destroy_queue();
        int sources = 0;
        obs_enum_sources([](void* p, obs_source_t*) { ++*static_cast<int*>(p); return true; }, &sources);
        Check(sources == 0, "Test scenes and their sources released before OBS shutdown");
        ShutdownTikTokSafety();
        obs_shutdown();
        std::cout << "PASS: real OBS scene composition, swapped/equal/shared audio tracks, separate signed feeds, duplicate prevention, startup failure, timeout, cancellation and cleanup\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

#include "dual-output-test.moc"
