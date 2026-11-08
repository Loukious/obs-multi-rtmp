#pragma once
#include "output-config.h"
#include <obs.h>
#include <QString>
class QWidget;
// OBS reloads persistent canvases without their video mix. Restore it from
// the saved layout before preview, Apply or Start; retain already-live video.
bool EnsureLayoutCanvasVideo(obs_canvas_t* canvas, const LayoutVideoConfig& config, QString* error);

class LayoutScene {
public:
    LayoutScene() = default;
    LayoutScene(const LayoutScene&) = delete;
    LayoutScene& operator=(const LayoutScene&) = delete;
    ~LayoutScene();
    bool Create(const LayoutVideoConfig& config, QString* error);
    // Preview the selected composition without switching a persistent
    // canvas's current scene, including when another output uses it.
    bool CreatePreview(const LayoutVideoConfig& config, QString* error);
    video_t* Video() const;
    void Render() const;
    bool SourceExists() const;
private:
    obs_canvas_t* canvas_ = nullptr;
    obs_scene_t* wrapper_ = nullptr;
    obs_source_t* source_ = nullptr;
    bool borrowedCanvas_ = false;
};
void ShowLayoutPreview(const LayoutVideoConfig& config, QWidget* parent);
