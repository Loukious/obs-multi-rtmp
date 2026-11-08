#pragma once
#include "account-info.h"
#include <QWidget>
#include <QTimer>
#include <future>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;

class AccountInfoWidget : public QWidget {
public:
    explicit AccountInfoWidget(AccountInfoLookup lookup = LookupAccountInfo, QWidget *parent = nullptr);
    ~AccountInfoWidget() override;
    void SetConfig(const OutputTargetConfig &config);
    void SetContentChangedCallback(std::function<void()> callback) { contentChanged_ = std::move(callback); }
    void SetDualAccessChangedCallback(std::function<void(std::optional<bool>)> callback) { dualAccessChanged_ = std::move(callback); }
    void SetDeviceProfileChangedCallback(std::function<void(const std::string&, const nlohmann::json&)> callback) { deviceProfileChanged_ = std::move(callback); }
private:
    void Refresh();
    void StartLookup();
    void Poll();
    const char *InputHint() const;
    AccountInfoLookup lookup_;
    std::function<void()> contentChanged_;
    OutputTargetConfig request_;
    std::string identity_;
    QLineEdit *username_;
    QWidget *streamlabsDetails_;
    QLineEdit *applicationStatus_, *canGoLive_;
    QLabel *status_;
    QLabel *dualStatus_;
    QWidget* dualDetails_;
    QLineEdit* canDualLayout_;
    std::function<void(std::optional<bool>)> dualAccessChanged_;
    std::function<void(const std::string&, const nlohmann::json&)> deviceProfileChanged_;
    QPushButton *refresh_;
    QTimer debounce_, poll_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<AccountInfoResult> result_;
    uint64_t generation_ = 0, runningGeneration_ = 0;
    bool pending_ = false;
};
