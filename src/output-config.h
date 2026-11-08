#pragma once

#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <memory>
#include <list>

#include <json.hpp>
#include "tiktok-live-settings.h"

struct VideoEncoderConfig {
    std::string id;
    std::string encoderId;
    int fpsDenumerator = 1;
    nlohmann::json encoderParams;
    std::optional<std::string> outputScene;
    std::optional<std::string> resolution;
};
using VideoEncoderConfigPtr = std::shared_ptr<VideoEncoderConfig>;

struct AudioTrackConfig {
    int mixer_track;
    int output_track;
};
using AudioTrackConfigPtr = std::shared_ptr<AudioTrackConfig>;

struct AudioEncoderConfig {
    std::string id;
    std::string encoderId;
    nlohmann::json encoderParams;
    int mixerId = 0;
    std::list<AudioTrackConfigPtr> audioTracks;
};
using AudioEncoderConfigPtr = std::shared_ptr<AudioEncoderConfig>; 


enum class ConnectionMethod {
    Custom,
    Streamlabs,
    TikTokLiveStudio,
};

// Layout settings belong to this target. Independent audio configurations keep
// distinct identities even when their codec, bitrate and selected mix match.
struct LayoutAudioConfig {
    std::string encoderId = "ffmpeg_aac";
    int mixer = 0;
    int bitrate = 160;
    nlohmann::json encoderParams = nlohmann::json::object();
};
struct LayoutVideoConfig {
    std::string sceneUuid, sceneName;
    std::string encoderId;
    int width = 1920, height = 1080;
    int bitrate = 6000;
    bool crop = false;
    LayoutAudioConfig audio;
    int fpsDivisor = 1;
    nlohmann::json encoderParams = nlohmann::json::object();
    std::string canvasUuid;
};
struct DualLayoutConfig {
    bool enabled = false;
    bool sameAudio = true;
    bool independentAudioInitialized = false;
    LayoutAudioConfig sharedAudio;
    LayoutVideoConfig portrait = {{}, {}, {}, 1080, 1920};
    LayoutVideoConfig landscape;
};

inline const LayoutAudioConfig& LayoutAudio(const DualLayoutConfig& config, bool portrait)
{
    return config.sameAudio ? config.sharedAudio : (portrait ? config.portrait.audio : config.landscape.audio);
}

struct OutputTargetConfig {
    std::string id;
    std::string name;
    std::string protocol = "RTMP";
    bool syncStart = false;
    bool syncStop = false;
    // When false, this target is skipped by "Start all" / sync-start and
    // StartStreaming() refuses to start it, without losing its saved
    // settings. Lets a target be temporarily excluded (e.g. a platform
    // that's down) without deleting and re-creating it.
    bool enabled = true;
    ConnectionMethod connectionMethod = ConnectionMethod::Custom;
    std::string streamlabsAccessToken;
    std::string rapidApiKey;
    std::string cookieFilePath;
    std::string tiktokClient = "pc";
    nlohmann::json tiktokDeviceProfiles = nlohmann::json::object();
    std::string tiktokTopicId;
    std::string tiktokGameId;
    std::string tiktokGameName;
    TikTokLiveSettings tiktokLiveSettings;
    bool monitorTikTokViolations = true;
    DualLayoutConfig dualLayout;

    bool UsesDualLayout() const { return connectionMethod == ConnectionMethod::TikTokLiveStudio && dualLayout.enabled; }
    std::string streamlabsTitle;
    std::string streamlabsCategory;
    std::optional<std::string> streamlabsCategoryId;
    bool streamlabsMatureContent = false;

    nlohmann::json serviceParam;
    nlohmann::json outputParam;

    std::optional<std::string> videoConfig;
    std::optional<std::string> audioConfig;

    const char* OutputProtocol() const {
        return connectionMethod == ConnectionMethod::Custom ? protocol.c_str() : "RTMP";
    }
};
using OutputTargetConfigPtr = std::shared_ptr<OutputTargetConfig>;


struct MultiOutputConfig {
public:
    std::list<OutputTargetConfigPtr> targets;
    std::list<VideoEncoderConfigPtr> videoConfig;
    std::list<AudioEncoderConfigPtr> audioConfig;
};

template<class T, class S>
inline T FindById(std::list<T>& list, const S& id) {
    for(auto& x: list) {
        if (x->id == id)
            return x;
    }
    return nullptr;
}


MultiOutputConfig& GlobalMultiOutputConfig();

void SaveMultiOutputConfig();

bool LoadMultiOutputConfig();

std::string GenerateId(MultiOutputConfig& config);

// Used by the dock's Export/Import Config buttons to back up or restore
// all targets (including stream keys) outside of OBS's own profile
// folder, since the plugin otherwise only ever saves into the current
// profile's obs-multi-rtmp.json.
std::string SerializeMultiOutputConfig(MultiOutputConfig& config);

// Returns std::nullopt only if `content` isn't valid JSON; a validly
// parsed but unexpectedly-shaped file yields an (possibly empty) config
// instead of failing, same tolerance as the profile loader.
std::optional<MultiOutputConfig> DeserializeMultiOutputConfig(const std::string& content);
