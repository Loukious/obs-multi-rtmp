#pragma once
#include "output-config.h"
#include <obs.h>
#include <QString>
#include <functional>

using PrimaryCanvasCreate = std::function<obs_canvas_t*(const char*, obs_video_info*, int)>;
using PrimaryCanvasRemove = std::function<void(obs_canvas_t*)>;
// Creates persistent frontend canvases only for unbound layouts. Existing
// canvases are retained; after OBS reloads them, their missing video mix is
// restored from the saved layout dimensions. Configured video is never resized.
bool PreparePrimaryCanvases(OutputTargetConfig& config, QString* error,
    PrimaryCanvasCreate create = {}, PrimaryCanvasRemove remove = {});
// Retire only plugin-generated canvases superseded by a successful edit.
void RemoveReplacedPrimaryCanvases(const OutputTargetConfig& before, const OutputTargetConfig& after,
    PrimaryCanvasRemove remove = {});
