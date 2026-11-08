#pragma once
#include "output-config.h"
#include "tiktok-native/live_safety.hpp"
#include <QObject>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <functional>
#include <future>
#include <memory>

struct TikTokSafetyBackend {
    std::function<TikTokSafetySnapshot(TikTokSafetyContext)> check = CheckTikTokViolations;
    std::function<TikTokAppealState(TikTokSafetyContext, const QJsonObject&)> load = LoadTikTokAppeal;
    std::function<TikTokAppealState(const TikTokAppealState&, const QString&, const QString&)> submit = SubmitTikTokAppeal;
};
class QDialog;
class QWidget;
class TikTokSafetyMonitor;
TikTokSafetyContext MakeTikTokSafetyContext(const OutputTargetConfig& config);
// Modeless, so a violation never blocks OBS's streaming controls.
QDialog* ShowTikTokSafety(TikTokSafetyContext context, const QVector<QJsonObject>& records = {},
    TikTokSafetyBackend backend = {}, QWidget* parent = nullptr);
void OpenTikTokSafety(const OutputTargetConfig& config, QWidget* parent);
void ShutdownTikTokSafety();
// Retain the monitor until pending results and one final disconnect check finish.
void FinishTikTokSafety(std::unique_ptr<TikTokSafetyMonitor> monitor);

class TikTokSafetyMonitor : public QObject {
public:
    using Notify = std::function<void(const TikTokSafetyContext&, const QVector<QJsonObject>&)>;
    TikTokSafetyMonitor(TikTokSafetyContext context, TikTokSafetyBackend backend = {}, Notify notify = {});
    ~TikTokSafetyMonitor() override;
    void Start();
    void Stop();
    bool Finish(std::function<void()> complete);
    void CheckNow();
    bool Checking() const { return result_.valid(); }
private:
    void Poll();
    TikTokSafetyContext context_;
    TikTokSafetyBackend backend_;
    Notify notify_;
    QTimer schedule_, poll_;
    QSet<QString> seen_;
    QString lastError_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<TikTokSafetySnapshot> result_;
    bool stopped_ = false;
    bool finishing_ = false, finalCheckPending_ = false;
    std::function<void()> complete_;
};
