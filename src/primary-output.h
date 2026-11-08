#pragma once
#include "output-config.h"
#include <obs.h>
#include <QString>
#include <functional>

constexpr const char* TikTokPrimaryServiceId = "tiktok_primary_service";
constexpr const char* TikTokPrimaryOutputId = "tiktok_primary_output";
std::string EncodePrimaryConfig(const OutputTargetConfig& config);
OutputTargetConfig DecodePrimaryConfig(const char* json);
void RegisterPrimaryTikTokOutput();
void StopPrimaryTikTokOutputs();
bool PrimaryTikTokBusy();
void ReleasePrimaryTikTokObjects();
// Called on the UI thread after generated device profiles are saved to the
// active service. The frontend persists the service if it still owns it.
void SetPrimaryServiceSaveCallback(std::function<void(obs_service_t*)> callback);
QString PrimaryTikTokStatus();
