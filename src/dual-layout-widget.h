#pragma once
#include "output-config.h"
#include <QWidget>
#include <QString>
#include <QVector>
#include <functional>

class QComboBox;
class QSpinBox;
class QCheckBox;
class QGroupBox;
class QLabel;

struct DualChoice { QString id, name, canvasId; };
struct DualGeneratedLayout { QString sceneId, canvasId, inputId, inputName; };
struct DualUiCatalog {
    QVector<DualChoice> scenes, videoEncoders, audioEncoders, canvases;
    QVector<DualGeneratedLayout> generated;
    QString currentSceneId;
};
DualUiCatalog ReadDualUiCatalog();
const char* ValidateDualLayout(const OutputTargetConfig& config);

class DualLayoutWidget : public QWidget {
public:
    explicit DualLayoutWidget(QWidget* parent = nullptr, std::function<DualUiCatalog()> catalog = ReadDualUiCatalog);
    void Load(const DualLayoutConfig& config);
    void Save(DualLayoutConfig& config) const;
    bool Enabled() const;
    void SetChangedCallback(std::function<void()> callback) { changed_ = std::move(callback); }
    std::function<void(const LayoutVideoConfig&)> preview;
    std::function<bool(const std::string&, nlohmann::json&, int&, bool)> editEncoder;
    QWidget* VideoSettings() const { return videoSettings_; }
    QWidget* AudioSettings() const { return audioSettings_; }
    void SetAccess(std::optional<bool> available, bool cameraLive = true);
    void SetNativeCanvasMode(bool enabled);
    bool Validate(const OutputTargetConfig& config);
    void ShowVideoError(const QString& message);
private:
    struct AudioFields { QWidget* widget; QComboBox *track, *encoder; QSpinBox* bitrate; nlohmann::json params = nlohmann::json::object(); };
    struct VideoFields { QGroupBox* group; QComboBox *scene, *encoder, *fit, *canvas; QWidget* canvasLabel; QSpinBox *width, *height, *bitrate, *fps; AudioFields audio; nlohmann::json params = nlohmann::json::object(); std::optional<LayoutVideoConfig> prepared; QString inputId; };
    AudioFields AddAudio(const QString& prefix, QWidget* parent);
    VideoFields AddVideo(const QString& prefix, const char* title, QWidget* parent);
    void RefreshCatalog();
    void Visibility();
    void UpdateBandwidth();
    static void LoadAudio(AudioFields& fields, const LayoutAudioConfig& config);
    static void SaveAudio(const AudioFields& fields, LayoutAudioConfig& config);
    static void LoadVideo(VideoFields& fields, const LayoutVideoConfig& config, const DualUiCatalog& catalog);
    static void SaveVideo(const VideoFields& fields, LayoutVideoConfig& config);
    QComboBox* mode_;
    QCheckBox* sameAudio_;
    QWidget* settings_;
    QWidget *videoSettings_, *audioSettings_;
    QGroupBox *portraitAudio_, *landscapeAudio_;
    bool canDual_ = false;
    bool nativeCanvasMode_ = false;
    bool pendingSavedDual_ = false;
    QLabel* bandwidth_;
    QLabel *videoError_, *audioError_;
    VideoFields portrait_, landscape_;
    AudioFields shared_;
    bool independentInitialized_ = false;
    std::function<DualUiCatalog()> catalog_;
    std::function<void()> changed_;
};
