#pragma once
#include <obs.hpp>
#include <cstring>

inline constexpr const char* GeneratedLayoutKey = "obs-multi-rtmp-generated-layout";

// Returns a new reference to the sole source scene used by our composition.
inline obs_source_t* GeneratedLayoutInput(obs_source_t* source)
{
    if (!source || !obs_source_is_scene(source) || obs_source_removed(source)) return nullptr;
    OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
    if (!canvas || (obs_canvas_get_flags(canvas) & MAIN)) return nullptr;
    OBSDataAutoRelease settings = obs_source_get_private_settings(source);
    const char* name = obs_source_get_name(source);
    const bool legacy = (!std::strcmp(name, "TikTok Portrait") || !std::strcmp(name, "TikTok Landscape")) &&
        !std::strcmp(name, obs_canvas_get_name(canvas));
    if (!obs_data_get_bool(settings, GeneratedLayoutKey) && !legacy) return nullptr;
    struct Items { int count = 0; obs_source_t* input = nullptr; } items;
    obs_scene_enum_items(obs_scene_from_source(source), [](obs_scene_t*, obs_sceneitem_t* item, void* data) {
        auto& result = *static_cast<Items*>(data); ++result.count;
        auto input = obs_sceneitem_get_source(item);
        if (obs_source_is_scene(input) && !obs_source_removed(input)) result.input = input;
        return true;
    }, &items);
    if (items.count != 1 || !items.input) return nullptr;
    OBSCanvasAutoRelease owner = obs_source_get_canvas(items.input);
    return owner && owner != canvas ? obs_source_get_ref(items.input) : nullptr;
}
