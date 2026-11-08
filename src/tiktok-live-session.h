#pragma once
#include "output-config.h"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

struct obs_output;
class TikTokLiveSession : public QObject {
public:
    TikTokLiveSession();
    ~TikTokLiveSession() override;
    bool Start(const OutputTargetConfig &config, obs_output *output,
        std::function<void(const nlohmann::json &)> saveDevices);
    // Dual order is explicit: portrait, then landscape.
    bool Start(const OutputTargetConfig &config, const std::vector<obs_output*>& outputs,
        std::function<void(const nlohmann::json &)> saveDevices);
    void ConfirmStarted();
    void CancelPreparation();
    void Stop();
    QString Error() const;
    std::string Server(bool landscape = false) const;
    std::string Key(bool landscape = false) const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
