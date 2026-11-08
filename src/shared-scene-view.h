#pragma once

#include <obs.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>

class SharedSceneView final
{
    OBSEncoder encoder_;
    obs_view_t* view_ = nullptr;
    video_t* video_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    std::mutex source_mutex_;

public:
    explicit SharedSceneView(obs_encoder_t* encoder)
        : encoder_(encoder)
    {
    }

    ~SharedSceneView()
    {
        if (!view_)
            return;

        obs_view_remove(view_);
        OBSSourceAutoRelease source = obs_view_get_source(view_, 0);
        if (source)
            obs_source_dec_active(source);
        obs_view_set_source(view_, 0, nullptr);
        obs_view_destroy(view_);
    }

    bool Initialize(obs_source_t* source)
    {
        if (!source)
            return false;

        obs_video_info info{};
        width_ = obs_source_get_width(source);
        height_ = obs_source_get_height(source);
        if (!width_ || !height_ || !obs_get_video_info(&info))
            return false;
        // Aitum custom-sized scenes and native OBS canvases can differ from
        // the main canvas. Render their full frame before encoder scaling.
        info.base_width = info.output_width = width_;
        info.base_height = info.output_height = height_;

        view_ = obs_view_create();
        if (!view_)
            return false;

        obs_view_set_source(view_, 0, source);
        obs_source_inc_active(source);
        video_ = obs_view_add2(view_, &info);
        return video_ != nullptr;
    }

    bool SetSource(obs_source_t* source)
    {
        if (!view_ || !source)
            return false;

        std::lock_guard<std::mutex> lock(source_mutex_);
        if (obs_source_get_width(source) != width_ || obs_source_get_height(source) != height_) {
            // Another output may still be using this encoder. Never replace
            // its video mix while it is live.
            if (obs_encoder_active(encoder_))
                return false;
            SharedSceneView replacement(encoder_);
            if (!replacement.Initialize(source))
                return false;
            obs_encoder_set_video(encoder_, replacement.Video());
            if (obs_encoder_parent_video(encoder_) != replacement.Video())
                return false;
            std::swap(view_, replacement.view_);
            std::swap(video_, replacement.video_);
            std::swap(width_, replacement.width_);
            std::swap(height_, replacement.height_);
            return true;
        }
        OBSSourceAutoRelease current = obs_view_get_source(view_, 0);
        if (current == source)
            return true;

        obs_view_set_source(view_, 0, source);
        if (current)
            obs_source_dec_active(current);
        obs_source_inc_active(source);
        return true;
    }

    video_t* Video() const
    {
        return video_;
    }
};

class SharedSceneViewManager final
{
    std::mutex mutex_;
    std::unordered_map<obs_encoder_t*, std::weak_ptr<SharedSceneView>> entries_;

public:
    std::shared_ptr<SharedSceneView> Acquire(obs_encoder_t* encoder, obs_source_t* source)
    {
        if (!encoder || !source)
            return {};

        std::unique_lock<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.expired())
                it = entries_.erase(it);
            else
                ++it;
        }

        auto it = entries_.find(encoder);
        if (it != entries_.end()) {
            auto entry = it->second.lock();
            if (entry) {
                lock.unlock();
                if (!entry->SetSource(source)) {
                    blog(LOG_ERROR, "[obs-multi-rtmp] Could not update the shared output scene.");
                    return {};
                }
                return entry;
            }
            entries_.erase(it);
        }

        if (obs_encoder_active(encoder)) {
            blog(LOG_ERROR, "[obs-multi-rtmp] Shared video encoder is active without a registered scene view.");
            return {};
        }

        auto entry = std::make_shared<SharedSceneView>(encoder);
        if (!entry->Initialize(source)) {
            lock.unlock();
            entry.reset();
            blog(LOG_ERROR, "[obs-multi-rtmp] Could not create the shared output scene view.");
            return {};
        }

        obs_encoder_set_video(encoder, entry->Video());
        if (obs_encoder_parent_video(encoder) != entry->Video()) {
            lock.unlock();
            entry.reset();
            blog(LOG_ERROR, "[obs-multi-rtmp] Could not bind the shared scene view to the video encoder.");
            return {};
        }

        entries_[encoder] = entry;
        return entry;
    }
};

inline SharedSceneViewManager& GetSharedSceneViewManager()
{
    static SharedSceneViewManager manager;
    return manager;
}

