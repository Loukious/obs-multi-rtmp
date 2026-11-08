#pragma once
#include "output-config.h"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <obs.h>

class DualOutputSession : public QObject {
public:
    explicit DualOutputSession(QObject* parent = nullptr, int startupTimeoutMs = 60000);
    ~DualOutputSession() override;
    bool Start(const OutputTargetConfig& config, std::function<void(const nlohmann::json&)> saveDevices);
    void Stop();
    bool Running() const;
    bool Live() const;
    QString Status() const;
    QString Error() const;
    obs_encoder_t* VideoEncoder(bool portrait) const;
    obs_encoder_t* AudioEncoder(bool portrait) const;
    uint64_t TotalBytes() const;
    int DroppedFrames() const;
private:
    struct State;
    std::unique_ptr<State> state_;
    void Poll();
    void Finish();
};
