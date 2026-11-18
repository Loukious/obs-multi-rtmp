#pragma once

#include <obs.h>

// Following a native canvas borrows its running video mix. It must never
// initialize/reset that mix or switch the canvas's program source.
inline bool ReadFollowCanvasVideo(obs_canvas_t* canvas, obs_video_info& info)
{
    return canvas && !obs_canvas_removed(canvas)
        && !(obs_canvas_get_flags(canvas) & EPHEMERAL)
        && obs_canvas_has_video(canvas) && obs_canvas_get_video(canvas)
        && obs_canvas_get_video_info(canvas, &info)
        && info.base_width && info.base_height && info.output_width && info.output_height;
}
