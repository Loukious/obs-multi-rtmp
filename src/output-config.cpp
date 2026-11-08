#include "output-config.h"
#include "pch.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <random>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <util/platform.h>
#include "json-util.hpp"


MultiOutputConfig& GlobalMultiOutputConfig()
{
    static MultiOutputConfig instance;
    return instance;
}


static nlohmann::json SaveLayoutAudio(const LayoutAudioConfig& c) {
    return {{"encoder", c.encoderId}, {"mixer", c.mixer}, {"bitrate", c.bitrate}, {"param", c.encoderParams}};
}
static nlohmann::json SaveLayoutVideo(const LayoutVideoConfig& c) {
    return {{"scene-uuid", c.sceneUuid}, {"scene-name", c.sceneName}, {"encoder", c.encoderId},
        {"width", c.width}, {"height", c.height}, {"bitrate", c.bitrate}, {"crop", c.crop}, {"audio", SaveLayoutAudio(c.audio)},
        {"fps-divisor", c.fpsDivisor}, {"param", c.encoderParams}, {"canvas-uuid", c.canvasUuid}};
}
static void LoadLayoutAudio(nlohmann::json j, LayoutAudioConfig& c) {
    c.encoderId = GetJsonField<std::string>(j, "encoder").value_or(c.encoderId);
    c.mixer = GetJsonField<int>(j, "mixer").value_or(c.mixer);
    c.encoderParams = GetJsonField<nlohmann::json>(j, "param").value_or(nlohmann::json::object());
    if (!c.encoderParams.is_object()) c.encoderParams = nlohmann::json::object();
    c.bitrate = GetJsonField<int>(j, "bitrate").value_or(c.bitrate);
}
static void LoadLayoutVideo(nlohmann::json j, LayoutVideoConfig& c) {
    c.canvasUuid = GetJsonField<std::string>(j, "canvas-uuid").value_or("");
    c.sceneUuid = GetJsonField<std::string>(j, "scene-uuid").value_or("");
    c.sceneName = GetJsonField<std::string>(j, "scene-name").value_or("");
    c.encoderId = GetJsonField<std::string>(j, "encoder").value_or(c.encoderId);
    c.width = GetJsonField<int>(j, "width").value_or(c.width);
    c.height = GetJsonField<int>(j, "height").value_or(c.height);
    c.bitrate = GetJsonField<int>(j, "bitrate").value_or(c.bitrate);
    c.crop = GetJsonField<bool>(j, "crop").value_or(c.crop);
    c.fpsDivisor = GetJsonField<int>(j, "fps-divisor").value_or(1);
    c.encoderParams = GetJsonField<nlohmann::json>(j, "param").value_or(nlohmann::json::object());
    if (!c.encoderParams.is_object()) c.encoderParams = nlohmann::json::object();
    LoadLayoutAudio(GetJsonField<nlohmann::json>(j, "audio").value_or(nlohmann::json::object()), c.audio);
}

static nlohmann::json SaveTarget(OutputTargetConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["name"] = config.name;
    json["protocol"] = config.protocol;
    json["service-param"] = config.serviceParam;
    json["output-param"] = config.outputParam;
    json["sync-start"] = config.syncStart;
    json["sync-stop"] = config.syncStop;
    json["enabled"] = config.enabled;
    const char* method = "custom";
    if (config.connectionMethod == ConnectionMethod::Streamlabs)
        method = "streamlabs";
    else if (config.connectionMethod == ConnectionMethod::TikTokLiveStudio)
        method = "tiktok-live-studio";
    json["connection-method"] = method;
    json["streamlabs-access-token"] = config.streamlabsAccessToken;
    json["rapidapi-key"] = config.rapidApiKey;
    json["cookie-file-path"] = config.cookieFilePath;
    json["tiktok-client"] = config.tiktokClient;
    json["tiktok-device-profiles"] = config.tiktokDeviceProfiles;
    json["tiktok-topic-id"] = config.tiktokTopicId;
    json["tiktok-game-id"] = config.tiktokGameId;
    json["tiktok-game-name"] = config.tiktokGameName;
    json["tiktok-allow-gifts"] = config.tiktokLiveSettings.allowGifts;
    json["tiktok-monitor-violations"] = config.monitorTikTokViolations;
    json["tiktok-allow-comments"] = config.tiktokLiveSettings.allowComments;
    json["tiktok-content-disclosure"] = config.tiktokLiveSettings.contentDisclosure;
    json["dual-layout"] = {{"enabled", config.dualLayout.enabled}, {"same-audio", config.dualLayout.sameAudio},
        {"independent-audio-initialized", config.dualLayout.independentAudioInitialized},
        {"shared-audio", SaveLayoutAudio(config.dualLayout.sharedAudio)},
        {"portrait", SaveLayoutVideo(config.dualLayout.portrait)}, {"landscape", SaveLayoutVideo(config.dualLayout.landscape)}};
    json["streamlabs-title"] = config.streamlabsTitle;
    json["streamlabs-category"] = config.streamlabsCategory;
    if (config.streamlabsCategoryId.has_value())
        json["streamlabs-category-id"] = *config.streamlabsCategoryId;
    json["streamlabs-mature-content"] = config.streamlabsMatureContent;
    if (config.videoConfig.has_value())
        json["video-config"] = *config.videoConfig;
    if (config.audioConfig.has_value())
        json["audio-config"] = *config.audioConfig;
    return json;
}

static nlohmann::json SaveVideoConfig(VideoEncoderConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["encoder"] = config.encoderId;
    json["param"] = config.encoderParams;
    if (config.outputScene.has_value())
        json["scene"] = *config.outputScene;
    if (config.resolution.has_value())
        json["resolution"] = *config.resolution;
    json["fps-denumerator"] = config.fpsDenumerator;
    return json;
}

static nlohmann::json SaveAudioTrackConfig(AudioTrackConfig &config) {
	nlohmann::json json;
	json["mixer_track"] = config.mixer_track;
	json["output_track"] = config.output_track;
	return json;
}

static nlohmann::json SaveAudioConfig(AudioEncoderConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["encoder"] = config.encoderId;
    json["param"] = config.encoderParams;
    json["mixerId"] = config.mixerId;


    nlohmann::json audio_tracks(nlohmann::json::value_t::array);
    for(auto& track: config.audioTracks) {
        audio_tracks.push_back(SaveAudioTrackConfig(*track));
    }

    json["audioTracks"] = audio_tracks;

    return json;
}

static std::string SaveMultiOutputConfig(MultiOutputConfig& config) {
    nlohmann::json json;

    std::unordered_set<std::string> videoconfig_in_use;
    std::unordered_set<std::string> audioconfig_in_use;

    int target_count = 0, videocfg_count = 0, audiocfg_count = 0;

    nlohmann::json targets(nlohmann::json::value_t::array);
    for(auto& target: config.targets) {
        targets.push_back(SaveTarget(*target));
        if (target->videoConfig.has_value())
            videoconfig_in_use.insert(*target->videoConfig);
        if (target->audioConfig.has_value())
            audioconfig_in_use.insert(*target->audioConfig);
        ++target_count;
    }

    nlohmann::json video_configs(nlohmann::json::value_t::array);
    for(auto& video_config: config.videoConfig) {
        if (videoconfig_in_use.find(video_config->id) != videoconfig_in_use.end())
            video_configs.push_back(SaveVideoConfig(*video_config));
        ++videocfg_count;
    }

    nlohmann::json audio_configs(nlohmann::json::value_t::array);
    for(auto& audio_config: config.audioConfig) {
        if (audioconfig_in_use.find(audio_config->id) != audioconfig_in_use.end())
            audio_configs.push_back(SaveAudioConfig(*audio_config));
        ++audiocfg_count;
    }

    json["targets"] = targets;
    json["video_configs"] = video_configs;
    json["audio_configs"] = audio_configs;

    blog(LOG_INFO, TAG "Save %d targets, %d video configs, %d audio configs", target_count, videocfg_count, audiocfg_count);

    return json.dump();
}




static OutputTargetConfigPtr LoadTargetConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};

    auto config = std::make_shared<OutputTargetConfig>();
    config->id = *id;
    config->name = GetJsonField<std::string>(json, "name").value_or("");
    config->protocol = GetJsonField<std::string>(json, "protocol").value_or("RTMP"); // for compatibility
    config->syncStart = GetJsonField<bool>(json, "sync-start").value_or(false);
    config->syncStop = GetJsonField<bool>(json, "sync-stop").value_or(config->syncStart);
    config->enabled = GetJsonField<bool>(json, "enabled").value_or(true);
    const auto method = GetJsonField<std::string>(json, "connection-method");
    const bool legacyStreamlabs = !method && GetJsonField<bool>(json, "streamlabs-token").value_or(false);
    if (method == "streamlabs" || legacyStreamlabs)
        config->connectionMethod = ConnectionMethod::Streamlabs;
    else if (method == "tiktok-live-studio")
        config->connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config->streamlabsAccessToken = GetJsonField<std::string>(json, "streamlabs-access-token").value_or("");
    config->rapidApiKey = GetJsonField<std::string>(json, "rapidapi-key").value_or("");
    config->cookieFilePath = GetJsonField<std::string>(json, "cookie-file-path").value_or("");
    config->tiktokClient = GetJsonField<std::string>(json, "tiktok-client").value_or("pc");
    if (config->tiktokClient != "mobile")
        config->tiktokClient = "pc";
    config->tiktokDeviceProfiles = GetJsonField<nlohmann::json>(json, "tiktok-device-profiles").value_or(nlohmann::json::object());
    config->tiktokTopicId = GetJsonField<std::string>(json, "tiktok-topic-id").value_or("");
    config->tiktokGameId = GetJsonField<std::string>(json, "tiktok-game-id").value_or("");
    config->tiktokGameName = GetJsonField<std::string>(json, "tiktok-game-name").value_or("");
    config->tiktokLiveSettings.allowGifts = GetJsonField<bool>(json, "tiktok-allow-gifts").value_or(true);
    config->monitorTikTokViolations = GetJsonField<bool>(json, "tiktok-monitor-violations").value_or(true);
    config->tiktokLiveSettings.allowComments = GetJsonField<bool>(json, "tiktok-allow-comments").value_or(true);
    config->tiktokLiveSettings.contentDisclosure = GetJsonField<std::string>(json, "tiktok-content-disclosure").value_or("off");
    if (!config->tiktokLiveSettings.Valid()) config->tiktokLiveSettings.contentDisclosure = "off";
    auto dual = GetJsonField<nlohmann::json>(json, "dual-layout").value_or(nlohmann::json::object());
    config->dualLayout.enabled = GetJsonField<bool>(dual, "enabled").value_or(false);
    config->dualLayout.sameAudio = GetJsonField<bool>(dual, "same-audio").value_or(true);
    config->dualLayout.independentAudioInitialized = GetJsonField<bool>(dual, "independent-audio-initialized").value_or(false);
    LoadLayoutAudio(GetJsonField<nlohmann::json>(dual, "shared-audio").value_or(nlohmann::json::object()), config->dualLayout.sharedAudio);
    LoadLayoutVideo(GetJsonField<nlohmann::json>(dual, "portrait").value_or(nlohmann::json::object()), config->dualLayout.portrait);
    LoadLayoutVideo(GetJsonField<nlohmann::json>(dual, "landscape").value_or(nlohmann::json::object()), config->dualLayout.landscape);
    if (!config->tiktokDeviceProfiles.is_object())
        config->tiktokDeviceProfiles = nlohmann::json::object();
    config->streamlabsTitle = GetJsonField<std::string>(json, "streamlabs-title").value_or("");
    config->streamlabsCategory = GetJsonField<std::string>(json, "streamlabs-category").value_or("");
    config->streamlabsCategoryId = GetJsonField<std::string>(json, "streamlabs-category-id");
    config->streamlabsMatureContent = GetJsonField<bool>(json, "streamlabs-mature-content").value_or(false);
    config->serviceParam = GetJsonField<nlohmann::json>(json, "service-param").value_or(nlohmann::json{});
    if (legacyStreamlabs) {
        if (config->streamlabsAccessToken.empty())
            config->streamlabsAccessToken = GetJsonField<std::string>(config->serviceParam, "password").value_or("");
        // This password was an API token, not RTMP authentication.
        if (config->serviceParam.is_object()) {
            config->serviceParam.erase("password");
            config->serviceParam["use_auth"] = false;
        }
    }
    config->outputParam = GetJsonField<nlohmann::json>(json, "output-param").value_or(nlohmann::json{});
    config->videoConfig = GetJsonField<std::string>(json, "video-config");
    config->audioConfig = GetJsonField<std::string>(json, "audio-config");

    return config;
}

static VideoEncoderConfigPtr LoadVideoConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};

    auto config = std::make_shared<VideoEncoderConfig>();
    config->id = *id;
    config->encoderId = GetJsonField<std::string>(json, "encoder").value_or("");
    config->outputScene = GetJsonField<std::string>(json, "scene");
    config->resolution = GetJsonField<std::string>(json, "resolution");
    config->fpsDenumerator = GetJsonField<int>(json, "fps-denumerator").value_or(1);
    config->encoderParams = GetJsonField<nlohmann::json>(json, "param").value_or(nlohmann::json{});

    return config;
}

static AudioTrackConfigPtr LoadAudioTrackConfig(nlohmann::json& json) {
    auto config = std::make_shared<AudioTrackConfig>();
    config->mixer_track = GetJsonField<int>(json, "mixer_track").value_or(0);
    config->output_track = GetJsonField<int>(json, "output_track").value_or(0);

    return config;
}

static AudioEncoderConfigPtr LoadAudioConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};
    
    auto config = std::make_shared<AudioEncoderConfig>();
    config->id = *id;
    config->encoderId = GetJsonField<std::string>(json, "encoder").value_or("");
    config->mixerId = GetJsonField<int>(json, "mixerId").value_or(0);
    config->encoderParams = GetJsonField<nlohmann::json>(json, "param").value_or(nlohmann::json{});

    auto it = json.find("audioTracks");
    if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
        for(auto& audio_track_json: *it) {
            if (audio_track_json.type() != nlohmann::json::value_t::object)
                continue;
            auto audio_track = LoadAudioTrackConfig(audio_track_json);
            if (audio_track)
                config->audioTracks.emplace_back(audio_track);
        }
    }

    return config;
}

static MultiOutputConfig LoadMultiOutputConfig(const std::string& content) {
    try {
        int target_count = 0, videocfg_count = 0, audiocfg_count = 0;

        auto json = nlohmann::json::parse(content);
        MultiOutputConfig config;
        auto it = json.find("targets");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& target_json: *it) {
                if (target_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto target = LoadTargetConfig(target_json);
                if (target)
                    config.targets.emplace_back(target);
                ++target_count;
            }
        }

        it = json.find("video_configs");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& video_enc_json: *it) {
                if (video_enc_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto video_enc = LoadVideoConfig(video_enc_json);
                if (video_enc) {
                    config.videoConfig.emplace_back(video_enc);
                }
                ++videocfg_count;
            }
        }

        it = json.find("audio_configs");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& audio_enc_json: *it) {
                if (audio_enc_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto audio_enc = LoadAudioConfig(audio_enc_json);
                if (audio_enc) {
                    config.audioConfig.emplace_back(audio_enc);
                }
                ++audiocfg_count;
            }
        }

        blog(LOG_INFO, TAG "Load %d targets, %d video configs, %d audio configs", target_count, videocfg_count, audiocfg_count);
        
        return config;
    }
    catch(const std::exception& e) {
        blog(LOG_ERROR, TAG "Fail to parse config json: %s", e.what());
        return {};
    }
}

std::string SerializeMultiOutputConfig(MultiOutputConfig& config) {
    return SaveMultiOutputConfig(config);
}

std::optional<MultiOutputConfig> DeserializeMultiOutputConfig(const std::string& content) {
    try {
        auto parsed = nlohmann::json::parse(content);
        (void)parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return LoadMultiOutputConfig(content);
}

void SaveMultiOutputConfig() {
    auto profiledir = obs_frontend_get_current_profile_path();
    if (profiledir) {
        std::string filename = profiledir;
        filename += "/obs-multi-rtmp.json";
        auto content = SaveMultiOutputConfig(GlobalMultiOutputConfig());
        os_quick_write_utf8_file_safe(filename.c_str(), content.c_str(), content.size(), true, "tmp", "bak");
        blog(LOG_INFO, TAG "Save config into %s", filename.c_str());
    }
    bfree(profiledir);
}


bool LoadMultiOutputConfig() {
    auto profiledir = obs_frontend_get_current_profile_path();
    bool ret = false;
    if (profiledir) {
        std::string filename = profiledir;
        filename += "/obs-multi-rtmp.json";
        auto content = os_quick_read_utf8_file(filename.c_str());
        if (content) {
            GlobalMultiOutputConfig() = LoadMultiOutputConfig(content);
            bfree(content);
            ret = true;
            blog(LOG_INFO, TAG "Load config from %s", filename.c_str());
        } else {
            blog(LOG_INFO, TAG "Load config from %s failed", filename.c_str());
        }
    }
    bfree(profiledir);
    return ret;
}


template<class T>
static bool has_id(T& container, const std::string& id) {
    for(auto& item: container) {
        if (item->id == id)
            return true;
    }
    return false;
}

std::string GenerateId(MultiOutputConfig& config) {
    static std::random_device rndgen;
    for(;;) {
        auto rndnum = rndgen();
        auto newid = std::to_string(rndnum);
        if (has_id(config.targets, newid))
            continue;
        if (has_id(config.audioConfig, newid))
            continue;
        if (has_id(config.videoConfig, newid))
            continue;
        return newid;
    }
}
