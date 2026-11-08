#include "primary-canvas.h"
#include "layout-scene.h"
#include "generated-layout.h"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/vec2.h>
#include <array>

void RemoveReplacedPrimaryCanvases(const OutputTargetConfig& before, const OutputTargetConfig& after, PrimaryCanvasRemove remove)
{
    if (!remove) remove = [](obs_canvas_t* canvas) { obs_frontend_remove_canvas(canvas); };
    for (const auto* old : {&before.dualLayout.portrait, &before.dualLayout.landscape}) {
        if (old->canvasUuid.empty() || old->canvasUuid == after.dualLayout.portrait.canvasUuid ||
            old->canvasUuid == after.dualLayout.landscape.canvasUuid) continue;
        OBSSourceAutoRelease scene = obs_get_source_by_uuid(old->sceneUuid.c_str());
        OBSSourceAutoRelease input = GeneratedLayoutInput(scene);
        OBSCanvasAutoRelease canvas = scene ? obs_source_get_canvas(scene) : nullptr;
        if (!input || !canvas || old->canvasUuid != obs_canvas_get_uuid(canvas)) continue;
        // Users can add scenes to a persistent canvas after we create it.
        // Retire only a canvas that still contains just our composition.
        struct Contents { obs_source_t* generated; bool exclusive = true; } contents{scene.Get()};
        obs_canvas_enum_scenes(canvas, [](void* data, obs_source_t* candidate) {
            auto& contents = *static_cast<Contents*>(data);
            if (candidate != contents.generated) contents.exclusive = false;
            return true;
        }, &contents);
        if (contents.exclusive) remove(canvas);
    }
}

bool PreparePrimaryCanvases(OutputTargetConfig& config, QString* error, PrimaryCanvasCreate create, PrimaryCanvasRemove remove)
{
    if (!config.UsesDualLayout()) return true;
    if (!create) create = obs_frontend_add_canvas;
    if (!remove) remove = [](obs_canvas_t* canvas) { obs_frontend_remove_canvas(canvas); };
    auto updated = config;
    std::array<OBSSource, 2> sources;
    std::array<OBSCanvas, 2> created;
    obs_video_info info{};
    if (!obs_get_video_info(&info)) { *error = "OBS video is unavailable."; return false; }
    for (size_t i = 0; i < 2; ++i) {
        const auto& layout = i == 0 ? config.dualLayout.portrait : config.dualLayout.landscape;
        OBSSourceAutoRelease source = obs_get_source_by_uuid(layout.sceneUuid.c_str());
        if (!source || !obs_source_is_scene(source) || obs_source_removed(source)) { *error = obs_module_text("Dual.Error.SceneMissing"); return false; }
        sources[i] = source.Get();
        if (layout.canvasUuid.empty()) continue;
        OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(layout.canvasUuid.c_str());
        OBSCanvasAutoRelease owner = obs_source_get_canvas(source);
        if (!canvas || owner != canvas) {
            *error = obs_module_text("Primary.InvalidCanvas"); return false;
        }
        if (!EnsureLayoutCanvasVideo(canvas, layout, error)) return false;
    }
    for (size_t i = 0; i < 2; ++i) {
        auto& layout = i == 0 ? updated.dualLayout.portrait : updated.dualLayout.landscape;
        if (!layout.canvasUuid.empty()) continue;
        info.base_width = info.output_width = layout.width;
        info.base_height = info.output_height = layout.height;
        const char* name = i == 0 ? "TikTok Portrait" : "TikTok Landscape";
        OBSCanvasAutoRelease canvas = create(name, &info, ACTIVATE | SCENE_REF);
        if (canvas) created[i] = canvas.Get();
        OBSSceneAutoRelease scene = canvas ? obs_canvas_scene_create(canvas, name) : nullptr;
        auto item = scene ? obs_scene_add(scene, sources[i]) : nullptr;
        if (!item) {
            for (const auto& owned : created) if (owned) remove(owned);
            *error = "Could not create native TikTok canvases."; return false;
        }
        OBSDataAutoRelease metadata = obs_source_get_private_settings(obs_scene_get_source(scene));
        obs_data_set_bool(metadata, GeneratedLayoutKey, true);
        vec2 bounds; vec2_set(&bounds, float(layout.width), float(layout.height));
        obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
        obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
        obs_sceneitem_set_bounds_type(item, layout.crop ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER);
        obs_sceneitem_set_bounds_crop(item, layout.crop);
        obs_sceneitem_set_bounds(item, &bounds);
        obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
        layout.canvasUuid = obs_canvas_get_uuid(canvas);
        layout.sceneUuid = obs_source_get_uuid(obs_scene_get_source(scene));
        layout.sceneName = obs_source_get_name(obs_scene_get_source(scene));
    }
    config = std::move(updated);
    return true;
}
