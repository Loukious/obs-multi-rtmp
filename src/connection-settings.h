#pragma once

#include "output-config.h"
#include "account-info.h"
#include "streamlabs-info.h"
#include "tiktok-game-info.h"
#include <QWidget>
#include <functional>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class AccountInfoWidget;
class StreamlabsCategoryWidget;
class TikTokTopicWidget;
class DualLayoutWidget;

// Returns a locale key, or nullptr when the selected method is configured.
const char* ValidateConnectionSettings(const OutputTargetConfig& config);

class ConnectionSettingsWidget : public QWidget {
public:
    explicit ConnectionSettingsWidget(QWidget* customSettings, QWidget* parent = nullptr,
        AccountInfoLookup lookup = LookupAccountInfo, StreamlabsCategoryLookup categoryLookup = SearchStreamlabsCategories,
        TikTokGameLookup gameLookup = LookupTikTokGames);
    void Load(const OutputTargetConfig& config);
    void Save(OutputTargetConfig& config) const;
    bool Validate();
    void SetMethodChangedCallback(std::function<void()> callback);
    void SetContentChangedCallback(std::function<void()> callback);
    bool UsesDualLayout() const;
    void SetLayoutPreview(std::function<void(const LayoutVideoConfig&)> callback);
    DualLayoutWidget* LayoutSettings() const { return dual_; }

private:
    QCheckBox* monitorViolations_;
    void UpdateVisibility();
    void SaveDeviceInputs();
    void LoadDeviceInputs();
    void RefreshAccountLookup();
    void UpdateDualAccess();
    std::optional<bool> dualAccess_;
    AccountInfoWidget *accountInfo_ = nullptr;
    QComboBox* method_;
    QWidget* customSettings_;
    QWidget* streamlabsSettings_;
    QWidget* liveStudioSettings_;
    QWidget* metadataSettings_;
    QLabel* categoryLabel_;
    QLineEdit* token_;
    QLineEdit* rapidApiKey_;
    QLineEdit* cookieFile_;
    QComboBox* client_;
    QWidget* mobileOptions_;
    QComboBox* broadcastType_;
    QComboBox* orientation_;
    QLabel* orientationLabel_;
    QLineEdit* deviceId_;
    QLineEdit* installId_;
    nlohmann::json deviceProfiles_ = nlohmann::json::object();
    std::string deviceClient_ = "pc";
    QLineEdit* title_;
    StreamlabsCategoryWidget* category_;
    TikTokTopicWidget* topics_;
    DualLayoutWidget* dual_;
    std::function<void()> contentChanged_;
    QCheckBox* matureContent_;
    QWidget* nativeLiveOptions_;
    QCheckBox *allowGifts_, *allowComments_;
    QComboBox* contentDisclosure_;
    QLabel* error_;
    std::function<void()> methodChanged_;
};
