#pragma once
#include "output-config.h"
#include <QString>
#include <functional>
class QDialog;
struct PrimarySettingsHost {
    std::function<OutputTargetConfig()> load;
    std::function<bool()> enabled;
    std::function<bool()> busy;
    std::function<bool(OutputTargetConfig&, QString*)> prepare;
    std::function<void(const OutputTargetConfig&, const OutputTargetConfig&)> rollbackPrepare;
    std::function<void(const OutputTargetConfig&, bool)> commit;
    nlohmann::json previousService;
};
bool AttachPrimaryTikTokSettings(QDialog* dialog, PrimarySettingsHost host);
void InstallPrimaryTikTokFrontend();
void ShutdownPrimaryTikTokFrontend();
