#pragma once

#include "output-config.h"
#include "generated-layout.h"
#include <obs.hpp>

inline bool IsSelectableOutputScene(obs_source_t* source)
{
    if (!source || !obs_source_is_scene(source) || obs_source_removed(source))
        return false;
    OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
    if (!canvas || obs_canvas_removed(canvas) || (obs_canvas_get_flags(canvas) & EPHEMERAL))
        return false;
    OBSDataAutoRelease settings = obs_source_get_private_settings(source);
    if (obs_data_get_bool(settings, GeneratedLayoutKey))
        return false;
    OBSSourceAutoRelease generated = GeneratedLayoutInput(source);
    return !generated;
}

// Returns a new source reference. UUID is authoritative: a missing scene must
// never be replaced with a different scene that happens to have the same name.
inline obs_source_t* ResolveConfiguredScene(const VideoEncoderConfig& config)
{
    if (!config.outputSceneUuid || config.outputSceneUuid->empty())
        return nullptr;
    OBSSourceAutoRelease source = obs_get_source_by_uuid(config.outputSceneUuid->c_str());
    return IsSelectableOutputScene(source) ? obs_source_get_ref(source) : nullptr;
}
