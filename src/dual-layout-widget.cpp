#include "dual-layout-widget.h"
#include "settings-validation.h"
#include "generated-layout.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <obs.hpp>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStandardItemModel>
#include <cstring>
#include <algorithm>

DualUiCatalog ReadDualUiCatalog()
{
    DualUiCatalog result;
    if (!obs_initialized()) return result;
    obs_enum_canvases([](void* data, obs_canvas_t* canvas) {
        if (obs_canvas_get_flags(canvas) & EPHEMERAL || obs_canvas_removed(canvas)) return true;
        if (!(obs_canvas_get_flags(canvas) & MAIN)) static_cast<DualUiCatalog*>(data)->canvases.push_back({QString::fromUtf8(obs_canvas_get_uuid(canvas)), QString::fromUtf8(obs_canvas_get_name(canvas))});
        obs_canvas_enum_scenes(canvas, [](void* inner, obs_source_t* scene) {
            auto& result = *static_cast<DualUiCatalog*>(inner);
            OBSCanvasAutoRelease canvas = obs_source_get_canvas(scene);
            OBSSourceAutoRelease input = GeneratedLayoutInput(scene);
            if (input) {
                result.generated.push_back({QString::fromUtf8(obs_source_get_uuid(scene)), QString::fromUtf8(obs_canvas_get_uuid(canvas)),
                    QString::fromUtf8(obs_source_get_uuid(input)), QString::fromUtf8(obs_source_get_name(input))});
                return true;
            }
            auto label = QString::fromUtf8(obs_source_get_name(scene));
            if (!(obs_canvas_get_flags(canvas) & MAIN)) label += " — " + QString::fromUtf8(obs_canvas_get_name(canvas));
            result.scenes.push_back({QString::fromUtf8(obs_source_get_uuid(scene)), label, QString::fromUtf8(obs_canvas_get_uuid(canvas))});
            return true;
        }, data);
        return true;
    }, &result);
    result.canvases.removeIf([&](const auto& canvas) {
        return std::any_of(result.generated.begin(), result.generated.end(), [&](const auto& generated) { return generated.canvasId == canvas.id; }) &&
            std::none_of(result.scenes.begin(), result.scenes.end(), [&](const auto& scene) { return scene.canvasId == canvas.id; });
    });
    OBSSourceAutoRelease current = obs_frontend_get_current_scene();
    if (current) result.currentSceneId = QString::fromUtf8(obs_source_get_uuid(current));
    const char* id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
        if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) continue;
        const char* codec = obs_get_encoder_codec(id);
        if (!codec) continue;
        const DualChoice choice{QString::fromUtf8(id), QString::fromUtf8(obs_encoder_get_display_name(id))};
        if (!strcmp(codec, "h264") || !strcmp(codec, "hevc")) result.videoEncoders.push_back(choice);
        else if (!strcmp(codec, "aac")) result.audioEncoders.push_back(choice);
    }
    return result;
}

namespace {
enum class InvalidField { Scene, Encoder, Width, Height, Bitrate, FPS, Track, AudioEncoder, AudioBitrate, Canvas };
struct LayoutIssue {
    const char* key = nullptr;
    bool portrait = true;
    InvalidField field = InvalidField::Scene;
};
LayoutIssue CheckLayout(const OutputTargetConfig& config)
{
    if (!config.UsesDualLayout()) return {};
    for (bool portrait : {true, false}) {
        const auto& v = portrait ? config.dualLayout.portrait : config.dualLayout.landscape;
        if (v.sceneUuid.empty()) return {"Dual.Error.Scene", portrait, InvalidField::Scene};
        if (v.encoderId.empty()) return {"Dual.Error.Encoder", portrait, InvalidField::Encoder};
        if (v.width < 64 || v.width > 4096 || v.width % 2) return {"Dual.Error.Resolution", portrait, InvalidField::Width};
        if (v.height < 64 || v.height > 4096 || v.height % 2 ||
            (portrait ? v.width >= v.height : v.width <= v.height)) return {"Dual.Error.Resolution", portrait, InvalidField::Height};
        if (v.bitrate < 100 || v.bitrate > 50000) return {"Dual.Error.Bitrate", portrait, InvalidField::Bitrate};
        if (v.fpsDivisor < 1 || v.fpsDivisor > 6) return {"Dual.Error.FPS", portrait, InvalidField::FPS};
        const auto& a = LayoutAudio(config.dualLayout, portrait);
        if (a.mixer < 0 || a.mixer >= MAX_AUDIO_MIXES) return {"Dual.Error.Audio", portrait, InvalidField::Track};
        if (a.encoderId.empty()) return {"Dual.Error.Audio", portrait, InvalidField::AudioEncoder};
        if (a.bitrate < 32 || a.bitrate > 320) return {"Dual.Error.Audio", portrait, InvalidField::AudioBitrate};
    }
    return {};
}
}

const char* ValidateDualLayout(const OutputTargetConfig& config) { return CheckLayout(config).key; }

bool DualLayoutWidget::Validate(const OutputTargetConfig& config)
{
    videoError_->hide(); audioError_->hide();
    auto issue = CheckLayout(config);
    if (!issue.key && config.UsesDualLayout()) {
        const auto catalog = catalog_();
        const auto contains = [](const QVector<DualChoice>& choices, const std::string& id) {
            return std::any_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.id == QString::fromStdString(id); });
        };
        for (bool portrait : {true, false}) {
            const auto& v = portrait ? config.dualLayout.portrait : config.dualLayout.landscape;
            const auto generated = std::find_if(catalog.generated.begin(), catalog.generated.end(), [&](const auto& entry) {
                return entry.sceneId == QString::fromStdString(v.sceneUuid) && entry.canvasId == QString::fromStdString(v.canvasUuid) &&
                    contains(catalog.scenes, entry.inputId.toStdString());
            });
            if (!contains(catalog.scenes, v.sceneUuid) && generated == catalog.generated.end()) issue = {"Dual.Error.SceneMissing", portrait, InvalidField::Scene};
            else if (!contains(catalog.videoEncoders, v.encoderId)) issue = {"Dual.Error.Encoder", portrait, InvalidField::Encoder};
            else if (!contains(catalog.audioEncoders, LayoutAudio(config.dualLayout, portrait).encoderId)) issue = {"Dual.Error.Audio", portrait, InvalidField::AudioEncoder};
            else if (nativeCanvasMode_ && !v.canvasUuid.empty() && !contains(catalog.canvases, v.canvasUuid) && generated == catalog.generated.end()) issue = {"Primary.InvalidCanvas", portrait, InvalidField::Canvas};
            if (issue.key) break;
        }
    }
    if (!issue.key) return true;
    auto& video = issue.portrait ? portrait_ : landscape_;
    auto& audio = config.dualLayout.sameAudio ? shared_ : video.audio;
    QWidget* field = nullptr;
    auto label = videoError_;
    switch (issue.field) {
    case InvalidField::Scene: field = video.scene; break;
    case InvalidField::Encoder: field = video.encoder; break;
    case InvalidField::Width: field = video.width; break;
    case InvalidField::Height: field = video.height; break;
    case InvalidField::Bitrate: field = video.bitrate; break;
    case InvalidField::FPS: field = video.fps; break;
    case InvalidField::Canvas: field = video.canvas; break;
    case InvalidField::Track: field = audio.track; label = audioError_; break;
    case InvalidField::AudioEncoder: field = audio.encoder; label = audioError_; break;
    case InvalidField::AudioBitrate: field = audio.bitrate; label = audioError_; break;
    }
    if (!field->isEnabled() && (issue.field == InvalidField::Width || issue.field == InvalidField::Height)) field = video.canvas;
    const char* layout = label == audioError_ && config.dualLayout.sameAudio ? "Dual.SharedAudio" : issue.portrait ? "Dual.Portrait" : "Dual.Landscape";
    label->setText(QString::fromUtf8(obs_module_text(layout)) + ": " + obs_module_text(issue.key)); label->show();
    RevealSettingsError(field, label);
    return false;
}

void DualLayoutWidget::ShowVideoError(const QString& message)
{
    videoError_->setText(message); videoError_->show();
    RevealSettingsError(videoError_, videoError_);
}

static QSpinBox* Number(QWidget* parent, const QString& name, int min, int max, int value)
{
    auto box = new QSpinBox(parent);
    box->setObjectName(name);
    box->setRange(min, max);
    box->setValue(value);
    return box;
}

DualLayoutWidget::AudioFields DualLayoutWidget::AddAudio(const QString& prefix, QWidget* parent)
{
    AudioFields f{};
    f.widget = new QWidget(parent);
    f.widget->setObjectName(prefix + "Audio");
    auto form = new QFormLayout(f.widget);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    f.track = new QComboBox(f.widget);
    f.track->setObjectName(prefix + "AudioTrack");
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i) f.track->addItem(QString::number(i + 1), i);
    f.encoder = new QComboBox(f.widget);
    f.encoder->setObjectName(prefix + "AudioEncoder");
    for (auto combo : {f.track, f.encoder}) {
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(18);
    }
    f.bitrate = Number(f.widget, prefix + "AudioBitrate", 32, 320, 160);
    form->addRow(obs_module_text("Dual.AudioTrack"), f.track);
    form->addRow(obs_module_text("Dual.AudioEncoder"), f.encoder);
    form->addRow(obs_module_text("Dual.AudioBitrate"), f.bitrate);
    auto advanced = new QPushButton(obs_module_text("Dual.AdvancedEncoder"), f.widget);
    advanced->setObjectName(prefix + "AudioAdvanced"); advanced->setAutoDefault(false);
    form->addRow(advanced);
    QObject::connect(advanced, &QPushButton::clicked, this, [this, prefix] {
        auto& a = prefix == "shared" ? shared_ : prefix == "portrait" ? portrait_.audio : landscape_.audio;
        int bitrate = a.bitrate->value();
        if (editEncoder && editEncoder(a.encoder->currentData().toString().toStdString(), a.params, bitrate, true)) {
            a.bitrate->setValue(bitrate); if (changed_) changed_();
        }
    });
    QObject::connect(f.encoder, &QComboBox::activated, this, [this, prefix] {
        (prefix == "shared" ? shared_ : prefix == "portrait" ? portrait_.audio : landscape_.audio).params = nlohmann::json::object();
    });
    return f;
}

DualLayoutWidget::VideoFields DualLayoutWidget::AddVideo(const QString& prefix, const char* title, QWidget* parent)
{
    VideoFields f{};
    f.group = new QGroupBox(obs_module_text(title), parent);
    f.group->setObjectName(prefix + "VideoSettings");
    auto form = new QFormLayout(f.group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    f.canvas = new QComboBox(f.group);
    f.canvas->setObjectName(prefix + "NativeCanvas");
    f.canvasLabel = new QLabel(obs_module_text("Primary.Canvas"), f.group);
    form->addRow(f.canvasLabel, f.canvas);
    f.canvas->hide(); f.canvasLabel->hide();
    f.scene = new QComboBox(f.group);
    f.scene->setObjectName(prefix + "Scene");
    f.encoder = new QComboBox(f.group);
    f.encoder->setObjectName(prefix + "VideoEncoder");
    f.width = Number(f.group, prefix + "Width", 64, 4096, prefix == "portrait" ? 1080 : 1920);
    f.height = Number(f.group, prefix + "Height", 64, 4096, prefix == "portrait" ? 1920 : 1080);
    f.bitrate = Number(f.group, prefix + "VideoBitrate", 100, 50000, 6000);
    f.fps = Number(f.group, prefix + "FPSDivisor", 1, 6, 1);
    f.fit = new QComboBox(f.group);
    f.fit->setObjectName(prefix + "Fit");
    f.fit->addItem(obs_module_text("Dual.Fit"), false);
    f.fit->addItem(obs_module_text("Dual.Crop"), true);
    form->addRow(obs_module_text("Scene"), f.scene);
    form->addRow(obs_module_text("Dual.Width"), f.width);
    form->addRow(obs_module_text("Dual.Height"), f.height);
    form->addRow(obs_module_text("Dual.Framing"), f.fit);
    auto show = new QPushButton(obs_module_text("Dual.Preview"), f.group);
    show->setObjectName(prefix + "Preview");
    show->setAutoDefault(false);
    QObject::connect(show, &QPushButton::clicked, this, [this, prefix] {
        if (preview) { LayoutVideoConfig config; SaveVideo(prefix == "portrait" ? portrait_ : landscape_, config); preview(config); }
    });
    form->addRow(QString(), show);
    form->addRow(obs_module_text("Encoder"), f.encoder);
    form->addRow(obs_module_text("Dual.VideoBitrate"), f.bitrate);
    f.fps->setToolTip(obs_module_text("Dual.FPSHint"));
    form->addRow(obs_module_text("Dual.FPSDivisor"), f.fps);
    auto advanced = new QPushButton(obs_module_text("Dual.AdvancedEncoder"), f.group);
    advanced->setObjectName(prefix + "VideoAdvanced"); advanced->setAutoDefault(false);
    form->addRow(advanced);
    QObject::connect(advanced, &QPushButton::clicked, this, [this, prefix] {
        auto& v = prefix == "portrait" ? portrait_ : landscape_;
        int bitrate = v.bitrate->value();
        if (editEncoder && editEncoder(v.encoder->currentData().toString().toStdString(), v.params, bitrate, false)) {
            v.bitrate->setValue(bitrate); if (changed_) changed_();
        }
    });
    QObject::connect(f.encoder, &QComboBox::activated, this, [this, prefix] {
        (prefix == "portrait" ? portrait_ : landscape_).params = nlohmann::json::object();
    });
    QObject::connect(f.canvas, &QComboBox::activated, this, [this, prefix] {
        auto& fields = prefix == "portrait" ? portrait_ : landscape_;
        OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(fields.canvas->currentData().toString().toUtf8().constData());
        obs_video_info info{};
        if (canvas && obs_canvas_get_video_info(canvas, &info)) { fields.width->setValue(info.output_width); fields.height->setValue(info.output_height); }
        fields.scene->setCurrentIndex(0);
        RefreshCatalog();
        if (fields.scene->count() > 1) fields.scene->setCurrentIndex(1);
    });
    return f;
}

DualLayoutWidget::DualLayoutWidget(QWidget* parent, std::function<DualUiCatalog()> catalog) : QWidget(parent), catalog_(std::move(catalog))
{
    auto layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->setContentsMargins(0, 0, 0, 0);
    auto form = new QFormLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName("tiktokLayout");
    mode_->addItem(obs_module_text("Dual.Single"), false);
    mode_->addItem(obs_module_text("Dual.Both"), true);
    form->addRow(obs_module_text("Dual.Layout"), mode_);
    layout->addLayout(form);
    settings_ = new QWidget(this);
    auto settings = new QVBoxLayout(settings_);
    settings->setAlignment(Qt::AlignTop);
    settings->setContentsMargins(0, 0, 0, 0);
    auto refresh = new QPushButton(obs_module_text("Dual.RefreshSources"), settings_);
    refresh->setObjectName("refreshDualSources");
    refresh->setAutoDefault(false);
    settings->addWidget(refresh);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] { RefreshCatalog(); });
    videoSettings_ = new QWidget(settings_);
    videoSettings_->setObjectName("dualVideoSettings");
    auto videoLayout = new QVBoxLayout(videoSettings_);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoError_ = new QLabel(videoSettings_); videoError_->setObjectName("dualVideoError");
    videoError_->setWordWrap(true); videoError_->setTextFormat(Qt::PlainText); videoError_->hide(); videoLayout->addWidget(videoError_);
    auto columns = new QHBoxLayout;
    videoLayout->addLayout(columns);
    columns->setSpacing(20);
    columns->setContentsMargins(0, 0, 0, 0);
    portrait_ = AddVideo("portrait", "Dual.Portrait", videoSettings_);
    landscape_ = AddVideo("landscape", "Dual.Landscape", videoSettings_);
    columns->addWidget(portrait_.group, 1, Qt::AlignTop);
    columns->addWidget(landscape_.group, 1, Qt::AlignTop);
    settings->addWidget(videoSettings_);
    audioSettings_ = new QWidget(settings_);
    audioSettings_->setObjectName("dualAudioSettings");
    auto audioLayout = new QVBoxLayout(audioSettings_);
    audioLayout->setSpacing(16);
    audioLayout->setContentsMargins(0, 0, 0, 0);
    audioLayout->setAlignment(Qt::AlignTop);
    audioError_ = new QLabel(audioSettings_); audioError_->setObjectName("dualAudioError");
    audioError_->setWordWrap(true); audioError_->setTextFormat(Qt::PlainText); audioError_->hide(); audioLayout->addWidget(audioError_);
    sameAudio_ = new QCheckBox(obs_module_text("Dual.SameAudio"), audioSettings_);
    sameAudio_->setObjectName("dualSameAudio");
    sameAudio_->setChecked(true);
    audioLayout->addWidget(sameAudio_);
    shared_ = AddAudio("shared", audioSettings_);
    audioLayout->addWidget(shared_.widget);
    auto audioColumns = new QHBoxLayout;
    audioColumns->setSpacing(20);
    portraitAudio_ = new QGroupBox(obs_module_text("Dual.Portrait"), audioSettings_);
    landscapeAudio_ = new QGroupBox(obs_module_text("Dual.Landscape"), audioSettings_);
    portrait_.audio = AddAudio("portrait", portraitAudio_); landscape_.audio = AddAudio("landscape", landscapeAudio_);
    (new QVBoxLayout(portraitAudio_))->addWidget(portrait_.audio.widget);
    (new QVBoxLayout(landscapeAudio_))->addWidget(landscape_.audio.widget);
    audioColumns->addWidget(portraitAudio_, 1, Qt::AlignTop); audioColumns->addWidget(landscapeAudio_, 1, Qt::AlignTop);
    audioLayout->addLayout(audioColumns);
    settings->addWidget(audioSettings_);
    bandwidth_ = new QLabel(settings_);
    bandwidth_->setWordWrap(true);
    settings->addWidget(bandwidth_);
    for (auto bitrate : {portrait_.bitrate, landscape_.bitrate, shared_.bitrate, portrait_.audio.bitrate, landscape_.audio.bitrate})
        QObject::connect(bitrate, &QSpinBox::valueChanged, this, [this] { UpdateBandwidth(); });
    auto hint = new QLabel(obs_module_text("Dual.AudioHint"), settings_);
    hint->setWordWrap(true);
    audioLayout->addWidget(hint);
    layout->addWidget(settings_);
    QObject::connect(mode_, &QComboBox::currentIndexChanged, this, [this] { Visibility(); });
    QObject::connect(mode_, &QComboBox::activated, this, [this] { pendingSavedDual_ = false; });
    QObject::connect(sameAudio_, &QCheckBox::toggled, this, [this](bool shared) {
        if (!shared && !independentInitialized_) {
            LayoutAudioConfig audio; SaveAudio(shared_, audio);
            LoadAudio(portrait_.audio, audio); LoadAudio(landscape_.audio, audio);
            independentInitialized_ = true;
        }
        Visibility();
    });
    RefreshCatalog();
    SetAccess(std::nullopt);
    Visibility();
}

static void Choices(QComboBox* combo, const QVector<DualChoice>& choices, bool blank)
{
    const auto id = combo->currentData().toString();
    const auto name = combo->currentData(Qt::UserRole + 1).isValid() ? combo->currentData(Qt::UserRole + 1).toString() : combo->currentText();
    QSignalBlocker block(combo);
    combo->clear();
    if (blank) combo->addItem(QString(), QString());
    for (const auto& item : choices) combo->addItem(item.name, item.id);
    int index = combo->findData(id);
    if (index < 0 && !id.isEmpty()) {
        combo->addItem(name + " (" + obs_module_text("Dual.Missing") + ")", id); index = combo->count() - 1;
        combo->setItemData(index, name, Qt::UserRole + 1);
    }
    if (index >= 0) combo->setCurrentIndex(index);
}

void DualLayoutWidget::RefreshCatalog()
{
    const auto catalog = catalog_();
    for (auto f : {&portrait_, &landscape_}) {
        Choices(f->canvas, catalog.canvases, true);
        f->canvas->setItemText(0, obs_module_text("Primary.NewCanvas"));
        QVector<DualChoice> scenes;
        const auto selected = nativeCanvasMode_ ? f->canvas->currentData().toString() : QString();
        for (const auto& scene : catalog.scenes) if (selected.isEmpty() || scene.canvasId == selected) scenes.push_back(scene);
        Choices(f->scene, scenes, true);
        f->scene->setItemText(0, obs_module_text("Dual.SelectScene"));
        f->width->setEnabled(selected.isEmpty()); f->height->setEnabled(selected.isEmpty()); f->fit->setEnabled(selected.isEmpty());
        Choices(f->encoder, catalog.videoEncoders, false);
        Choices(f->audio.encoder, catalog.audioEncoders, false);
    }
    Choices(shared_.encoder, catalog.audioEncoders, false);
}

static void Select(QComboBox* box, const QString& id, const QString& name = {})
{
    int index = box->findData(id);
    if (index < 0 && !id.isEmpty()) {
        const auto label = name.isEmpty() ? id : name;
        box->addItem(label + " (" + obs_module_text("Dual.Missing") + ")", id); index = box->count() - 1;
        box->setItemData(index, label, Qt::UserRole + 1);
    }
    if (index >= 0) box->setCurrentIndex(index);
}
void DualLayoutWidget::LoadAudio(AudioFields& f, const LayoutAudioConfig& c)
{
    f.params = c.encoderParams;
    Select(f.encoder, QString::fromStdString(c.encoderId)); f.track->setCurrentIndex(f.track->findData(c.mixer)); f.bitrate->setValue(c.bitrate);
}
void DualLayoutWidget::SaveAudio(const AudioFields& f, LayoutAudioConfig& c)
{
    c.encoderParams = f.params;
    c.encoderId = f.encoder->currentData().toString().toStdString(); c.mixer = f.track->currentIndex() < 0 ? -1 : f.track->currentData().toInt(); c.bitrate = f.bitrate->value();
}
void DualLayoutWidget::LoadVideo(VideoFields& f, const LayoutVideoConfig& c, const DualUiCatalog& catalog)
{
    f.prepared.reset(); f.inputId.clear();
    auto sceneId = QString::fromStdString(c.sceneUuid), sceneName = QString::fromStdString(c.sceneName);
    auto canvasId = QString::fromStdString(c.canvasUuid);
    for (const auto& generated : catalog.generated) {
        if (generated.sceneId != sceneId || generated.canvasId != canvasId) continue;
        f.prepared = c; f.inputId = generated.inputId;
        sceneId = generated.inputId; sceneName = generated.inputName; canvasId.clear(); break;
    }
    if (sceneId.isEmpty()) {
        sceneId = catalog.currentSceneId;
        if (sceneId.isEmpty() && catalog.scenes.size() == 1) sceneId = catalog.scenes.front().id;
    }
    Select(f.canvas, canvasId);
    f.params = c.encoderParams; f.fps->setValue(c.fpsDivisor);
    Select(f.scene, sceneId, sceneName);
    Select(f.encoder, QString::fromStdString(c.encoderId));
    f.width->setValue(c.width); f.height->setValue(c.height); f.bitrate->setValue(c.bitrate); f.fit->setCurrentIndex(c.crop ? 1 : 0); LoadAudio(f.audio, c.audio);
}
void DualLayoutWidget::SaveVideo(const VideoFields& f, LayoutVideoConfig& c)
{
    c.canvasUuid = f.canvas->currentData().toString().toStdString();
    c.encoderParams = f.params; c.fpsDivisor = f.fps->value();
    c.sceneUuid = f.scene->currentData().toString().toStdString();
    c.sceneName = (f.scene->currentData(Qt::UserRole + 1).isValid() ? f.scene->currentData(Qt::UserRole + 1).toString() : f.scene->currentText()).toStdString();
    c.encoderId = f.encoder->currentData().toString().toStdString();
    c.width = f.width->value(); c.height = f.height->value(); c.bitrate = f.bitrate->value(); c.crop = f.fit->currentData().toBool(); SaveAudio(f.audio, c.audio);
    // The form selects an input scene; the transport retains its prepared
    // output only while that scene and its composition are unchanged.
    if (f.prepared && c.canvasUuid.empty() && f.scene->currentData().toString() == f.inputId &&
        c.width == f.prepared->width && c.height == f.prepared->height && c.crop == f.prepared->crop) {
        c.sceneUuid = f.prepared->sceneUuid; c.sceneName = f.prepared->sceneName; c.canvasUuid = f.prepared->canvasUuid;
    }
}
void DualLayoutWidget::Load(const DualLayoutConfig& c)
{
    QSignalBlocker a(mode_), b(sameAudio_);
    pendingSavedDual_ = c.enabled && !canDual_;
    mode_->setCurrentIndex(c.enabled && canDual_ ? 1 : 0); sameAudio_->setChecked(c.sameAudio); independentInitialized_ = c.independentAudioInitialized;
    const auto catalog = catalog_();
    // A previous canvas selection may have filtered the input list.
    Choices(portrait_.scene, catalog.scenes, true); Choices(landscape_.scene, catalog.scenes, true);
    LoadVideo(portrait_, c.portrait, catalog); LoadVideo(landscape_, c.landscape, catalog); LoadAudio(shared_, c.sharedAudio); RefreshCatalog(); Visibility();
}
void DualLayoutWidget::Save(DualLayoutConfig& c) const
{
    c.enabled = Enabled() || pendingSavedDual_; c.sameAudio = sameAudio_->isChecked(); c.independentAudioInitialized = independentInitialized_;
    SaveVideo(portrait_, c.portrait); SaveVideo(landscape_, c.landscape); SaveAudio(shared_, c.sharedAudio);
}
bool DualLayoutWidget::Enabled() const { return mode_->currentData().toBool(); }
void DualLayoutWidget::SetNativeCanvasMode(bool enabled) {
    nativeCanvasMode_ = enabled;
    for (auto f : {&portrait_, &landscape_}) { f->canvas->setVisible(enabled); f->canvasLabel->setVisible(enabled); }
    RefreshCatalog();
}
void DualLayoutWidget::Visibility()
{
    settings_->setVisible(Enabled()); shared_.widget->setVisible(sameAudio_->isChecked());
    videoSettings_->setVisible(Enabled()); audioSettings_->setVisible(Enabled());
    portraitAudio_->setVisible(!sameAudio_->isChecked()); landscapeAudio_->setVisible(!sameAudio_->isChecked());
    portrait_.audio.widget->setVisible(!sameAudio_->isChecked()); landscape_.audio.widget->setVisible(!sameAudio_->isChecked());
    UpdateBandwidth();
    if (changed_) changed_();
}
void DualLayoutWidget::UpdateBandwidth()
{
    const int audio = sameAudio_->isChecked() ? 2 * shared_.bitrate->value() : portrait_.audio.bitrate->value() + landscape_.audio.bitrate->value();
    bandwidth_->setText(QString::fromUtf8(obs_module_text("Dual.Bandwidth")).arg(portrait_.bitrate->value() + landscape_.bitrate->value() + audio));
}
void DualLayoutWidget::SetAccess(std::optional<bool> available, bool cameraLive)
{
    canDual_ = available.value_or(false) && cameraLive;
    auto item = static_cast<QStandardItemModel*>(mode_->model())->item(1);
    item->setEnabled(canDual_);
    item->setToolTip(obs_module_text(!cameraLive ? "Dual.Error.CameraOnly" : !available.has_value() ? "Dual.AccessUnknown" :
        !*available ? "Dual.AccessLocked" : "Dual.AccessAvailable"));
    mode_->setToolTip(item->toolTip());
    if (!canDual_ && Enabled()) mode_->setCurrentIndex(0);
    if (canDual_ && pendingSavedDual_) mode_->setCurrentIndex(1);
    if (available.has_value() || !cameraLive) pendingSavedDual_ = false;
}
