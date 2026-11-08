#include "connection-settings.h"
#include "connection-login.h"
#include "rapidapi-quota-widget.h"
#include "tiktok-safety.h"
#include "settings-validation.h"
#include "account-info-widget.h"
#include "streamlabs-category-widget.h"
#include "tiktok-topic-widget.h"
#include "dual-layout-widget.h"
#include "tiktok-native/tiktok_studio_topics.hpp"
#include "json-util.hpp"
#include <algorithm>
#include <obs-module.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

const char* ValidateConnectionSettings(const OutputTargetConfig& config)
{
    if (config.connectionMethod == ConnectionMethod::Streamlabs) {
        if (QString::fromStdString(config.streamlabsAccessToken).trimmed().isEmpty())
            return "Error.StreamlabsToken";
    } else if (config.connectionMethod == ConnectionMethod::TikTokLiveStudio) {
        if (!config.tiktokLiveSettings.Valid()) return "Error.ContentDisclosure";
        if (QString::fromStdString(config.rapidApiKey).trimmed().isEmpty())
            return "Error.RapidApiKey";
        const auto path = QString::fromStdString(config.cookieFilePath);
        if (path.trimmed().isEmpty())
            return "Error.CookieFileRequired";
        QFile file(path);
        if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
            return "Error.CookieFileUnreadable";
        auto profiles = config.tiktokDeviceProfiles;
        auto profile = GetJsonField<nlohmann::json>(profiles, config.tiktokClient.c_str()).value_or(nlohmann::json::object());
        const auto device = GetJsonField<std::string>(profile, "device_id").value_or("");
        const auto install = GetJsonField<std::string>(profile, "install_id").value_or("");
        const auto validId = [](const std::string& id) {
            return !id.empty() && id.find_first_not_of("0123456789") == std::string::npos && id.find_first_not_of('0') != std::string::npos;
        };
        if ((!device.empty() || !install.empty()) && (!validId(device) || !validId(install)))
            return "Error.TikTokDeviceIds";
        const auto topic = QString::fromStdString(config.tiktokTopicId);
        const auto& topics = tiktok_studio_topics();
        if (std::none_of(topics.begin(), topics.end(), [&](const auto& item) { return item.id == topic; }))
            return "Error.TikTokTopic";
        const auto broadcast = GetJsonField<std::string>(profile, "broadcast_type").value_or("video");
        if (config.UsesDualLayout() && config.tiktokClient == "mobile" && broadcast != "video")
            return "Dual.Error.CameraOnly";
        if (config.tiktokClient == "mobile" && broadcast != "video" && broadcast != "screen_record")
            return "Error.MobileBroadcast";
        if (config.tiktokClient == "mobile" && broadcast == "screen_record") {
            const auto orientation = GetJsonField<std::string>(profile, "orientation").value_or("");
            if (orientation != "1" && orientation != "2") return "Error.MobileOrientation";
        }
        if (topic == "5" && (config.tiktokClient != "mobile" || broadcast == "screen_record") && !validId(config.tiktokGameId))
            return "Error.TikTokGame";
    }
    return ValidateDualLayout(config);
}

static QWidget* PasswordField(QLineEdit*& field, const char* name, QWidget* parent)
{
    auto container = new QWidget(parent);
    auto layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit(container);
    field->setObjectName(name);
    field->setEchoMode(QLineEdit::Password);
    auto show = new QPushButton(obs_module_text("Connection.Show"), container);
    show->setCheckable(true);
    show->setAutoDefault(false);
    QObject::connect(show, &QPushButton::toggled, field, [field, show](bool visible) {
        field->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        show->setText(obs_module_text(visible ? "Connection.Hide" : "Connection.Show"));
    });
    layout->addWidget(field, 1);
    layout->addWidget(show);
    return container;
}

ConnectionSettingsWidget::ConnectionSettingsWidget(QWidget* customSettings, QWidget* parent, AccountInfoLookup lookup,
    StreamlabsCategoryLookup categoryLookup, TikTokGameLookup gameLookup)
    : QWidget(parent), customSettings_(customSettings)
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignTop);
    auto methodLayout = new QFormLayout();
    method_ = new QComboBox(this);
    method_->setObjectName("connectionMethod");
    method_->addItem(obs_module_text("Connection.Custom"), static_cast<int>(ConnectionMethod::Custom));
    method_->addItem(obs_module_text("Connection.Streamlabs"), static_cast<int>(ConnectionMethod::Streamlabs));
    method_->addItem(obs_module_text("Connection.TikTokLiveStudio"), static_cast<int>(ConnectionMethod::TikTokLiveStudio));
    methodLayout->addRow(obs_module_text("Connection.Method"), method_);
    layout->addLayout(methodLayout);
    layout->addWidget(customSettings_);

    streamlabsSettings_ = new QWidget(this);
    streamlabsSettings_->setObjectName("streamlabsSettings");
    auto streamlabs = new QFormLayout(streamlabsSettings_);
    streamlabs->setContentsMargins(0, 0, 0, 0);
    streamlabs->addRow(obs_module_text("Connection.StreamlabsToken"), PasswordField(token_, "streamlabsAccessToken", streamlabsSettings_));
    auto getToken = new QPushButton(obs_module_text("GetStreamlabsToken"), streamlabsSettings_);
    getToken->setAutoDefault(false);
    QObject::connect(getToken, &QPushButton::clicked, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/Loukious/StreamLabsTikTokStreamKeyGenerator"));
    });
    streamlabs->addRow(QString(), getToken);
    auto detect = new QPushButton(obs_module_text("Login.DetectStreamlabs"), streamlabsSettings_);
    detect->setObjectName("detectStreamlabsAccount"); detect->setAutoDefault(false);
    QObject::connect(detect, &QPushButton::clicked, this, [this] {
        const auto token = DetectStreamlabsDesktopAccount(this);
        if (!token.isEmpty()) { token_->setText(token); if (contentChanged_) contentChanged_(); }
    });
    streamlabs->addRow(QString(), detect);
    auto info = new QLabel(obs_module_text("Connection.AutomaticServer"), streamlabsSettings_);
    info->setWordWrap(true);
    streamlabs->addRow(info);
    layout->addWidget(streamlabsSettings_);

    liveStudioSettings_ = new QWidget(this);
    liveStudioSettings_->setObjectName("liveStudioSettings");
    auto liveStudio = new QFormLayout(liveStudioSettings_);
    liveStudio->setContentsMargins(0, 0, 0, 0);
    client_ = new QComboBox(liveStudioSettings_);
    client_->setObjectName("tiktokClient");
    client_->addItem(obs_module_text("Connection.Desktop"), "pc");
    client_->addItem(obs_module_text("Connection.Mobile"), "mobile");
    liveStudio->addRow(obs_module_text("Connection.Client"), client_);
    mobileOptions_ = new QWidget(liveStudioSettings_);
    auto mobileOptions = new QFormLayout(mobileOptions_);
    mobileOptions->setContentsMargins(0, 0, 0, 0);
    broadcastType_ = new QComboBox(mobileOptions_);
    broadcastType_->setObjectName("tiktokBroadcastType");
    broadcastType_->addItem(obs_module_text("Connection.CameraLive"), "video");
    broadcastType_->addItem(obs_module_text("Connection.ScreenCapture"), "screen_record");
    mobileOptions->addRow(obs_module_text("Connection.BroadcastType"), broadcastType_);
    orientation_ = new QComboBox(mobileOptions_);
    orientation_->setObjectName("tiktokOrientation");
    orientation_->addItem(obs_module_text("Connection.Portrait"), "1");
    orientation_->addItem(obs_module_text("Connection.Landscape"), "2");
    orientationLabel_ = new QLabel(obs_module_text("Connection.Orientation"), mobileOptions_);
    mobileOptions->addRow(orientationLabel_, orientation_);
    liveStudio->addRow(mobileOptions_);
    liveStudio->addRow(obs_module_text("Connection.RapidApiKey"), PasswordField(rapidApiKey_, "rapidApiKey", liveStudioSettings_));
    auto quota = new RapidApiQuotaWidget(liveStudioSettings_);
    liveStudio->addRow(quota);
    QObject::connect(rapidApiKey_, &QLineEdit::textChanged, quota, &RapidApiQuotaWidget::SetApiKey);
    auto cookieRow = new QWidget(liveStudioSettings_);
    auto cookieLayout = new QHBoxLayout(cookieRow);
    cookieLayout->setContentsMargins(0, 0, 0, 0);
    cookieFile_ = new QLineEdit(cookieRow);
    cookieFile_->setObjectName("cookieFilePath");
    auto browse = new QPushButton(obs_module_text("Connection.Browse"), cookieRow);
    browse->setAutoDefault(false);
    browse->setObjectName("browseCookieFile");
    QObject::connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, obs_module_text("Connection.ChooseCookieFile"),
            cookieFile_->text(), obs_module_text("Connection.CookieFileFilter"));
        if (!path.isEmpty())
            cookieFile_->setText(path);
    });
    cookieLayout->addWidget(cookieFile_, 1);
    cookieLayout->addWidget(browse);
    liveStudio->addRow(obs_module_text("Connection.CookieFile"), cookieRow);
    auto qrLogin = new QPushButton(obs_module_text("Login.QrTitle"), liveStudioSettings_);
    qrLogin->setObjectName("tiktokQrLogin"); qrLogin->setAutoDefault(false);
    QObject::connect(qrLogin, &QPushButton::clicked, this, [this] {
        OutputTargetConfig config; Save(config);
        if (LoginTikTokWithQr(this, config)) { Load(config); if (contentChanged_) contentChanged_(); }
    });
    liveStudio->addRow(QString(), qrLogin);
    monitorViolations_ = new QCheckBox(obs_module_text("Safety.Monitor"), liveStudioSettings_);
    monitorViolations_->setObjectName("monitorTikTokViolations");
    monitorViolations_->setChecked(true);
    monitorViolations_->setToolTip(obs_module_text("Safety.PollingInfo"));
    liveStudio->addRow(monitorViolations_);
    auto safety = new QPushButton(obs_module_text("Safety.Check"), liveStudioSettings_);
    safety->setObjectName("checkTikTokViolations"); safety->setAutoDefault(false);
    QObject::connect(safety, &QPushButton::clicked, this, [this] { OutputTargetConfig config; Save(config); OpenTikTokSafety(config, this); });
    QObject::connect(monitorViolations_, &QCheckBox::toggled, this, [this] { if (contentChanged_) contentChanged_(); });
    liveStudio->addRow(QString(), safety);
    deviceId_ = new QLineEdit(liveStudioSettings_);
    deviceId_->setObjectName("tiktokDeviceId");
    installId_ = new QLineEdit(liveStudioSettings_);
    installId_->setObjectName("tiktokInstallId");
    deviceId_->setPlaceholderText(obs_module_text("Connection.AutomaticDevice"));
    installId_->setPlaceholderText(obs_module_text("Connection.AutomaticDevice"));
    liveStudio->addRow(obs_module_text("Connection.DeviceId"), deviceId_);
    liveStudio->addRow(obs_module_text("Connection.InstallId"), installId_);
    auto deviceInfo = new QLabel(obs_module_text("Connection.DeviceInfo"), liveStudioSettings_);
    deviceInfo->setWordWrap(true);
    liveStudio->addRow(deviceInfo);
    QObject::connect(client_, &QComboBox::currentIndexChanged, this, [this] {
        SaveDeviceInputs();
        deviceClient_ = client_->currentData().toString().toStdString();
        LoadDeviceInputs();
        UpdateVisibility();
    });
    layout->addWidget(liveStudioSettings_);
    accountInfo_ = new AccountInfoWidget(std::move(lookup), this);
    accountInfo_->SetDeviceProfileChangedCallback([this](const std::string& client, const nlohmann::json& profile) {
        deviceProfiles_[client] = profile;
        auto& stored = deviceProfiles_[client];
        if (client == deviceClient_) {
            QSignalBlocker deviceBlock(deviceId_), installBlock(installId_);
            deviceId_->setText(QString::fromStdString(GetJsonField<std::string>(stored, "device_id").value_or("")));
            installId_->setText(QString::fromStdString(GetJsonField<std::string>(stored, "install_id").value_or("")));
        }
    });
    layout->addWidget(accountInfo_);

    metadataSettings_ = new QWidget(this);
    auto metadata = new QFormLayout(metadataSettings_);
    metadata->setContentsMargins(0, 0, 0, 0);
    title_ = new QLineEdit(metadataSettings_);
    title_->setObjectName("tiktokTitle");
    category_ = new StreamlabsCategoryWidget(std::move(categoryLookup), metadataSettings_);
    metadata->addRow(obs_module_text("StreamlabsTitle"), title_);
    categoryLabel_ = new QLabel(obs_module_text("StreamlabsCategory"), metadataSettings_);
    metadata->addRow(categoryLabel_, category_);
    topics_ = new TikTokTopicWidget(std::move(gameLookup), metadataSettings_);
    metadata->addRow(topics_);
    matureContent_ = new QCheckBox(obs_module_text("StreamlabsMatureContent"), metadataSettings_);
    metadata->addRow(QString(), matureContent_);
    nativeLiveOptions_ = new QWidget(metadataSettings_);
    nativeLiveOptions_->setObjectName("tiktokLiveOptions");
    auto liveOptions = new QFormLayout(nativeLiveOptions_);
    liveOptions->setContentsMargins(0, 0, 0, 0);
    auto permissions = new QWidget(nativeLiveOptions_);
    auto permissionsLayout = new QHBoxLayout(permissions);
    permissionsLayout->setContentsMargins(0, 0, 0, 0);
    allowGifts_ = new QCheckBox(obs_module_text("Live.AllowGifts"), permissions);
    allowGifts_->setObjectName("tiktokAllowGifts"); allowGifts_->setChecked(true);
    allowComments_ = new QCheckBox(obs_module_text("Live.AllowComments"), permissions);
    allowComments_->setObjectName("tiktokAllowComments"); allowComments_->setChecked(true);
    permissionsLayout->addWidget(allowGifts_); permissionsLayout->addWidget(allowComments_); permissionsLayout->addStretch();
    liveOptions->addRow(permissions);
    contentDisclosure_ = new QComboBox(nativeLiveOptions_);
    contentDisclosure_->setObjectName("tiktokContentDisclosure");
    for (auto [label, value] : {std::pair{"Live.DisclosureOff", "off"}, {"Live.DisclosureOwn", "own_brand"},
        {"Live.DisclosureThirdParty", "third_party"}, {"Live.DisclosureBoth", "both"}})
        contentDisclosure_->addItem(obs_module_text(label), value);
    contentDisclosure_->setToolTip(obs_module_text("Live.DisclosureHint"));
    liveOptions->addRow(obs_module_text("Live.ContentDisclosure"), contentDisclosure_);
    auto startupHint = new QLabel(obs_module_text("Live.NewRoomsOnly"), nativeLiveOptions_);
    startupHint->setWordWrap(true); liveOptions->addRow(startupHint);
    metadata->addRow(nativeLiveOptions_);
    const auto liveOptionsChanged = [this] { if (contentChanged_) contentChanged_(); };
    QObject::connect(allowGifts_, &QCheckBox::toggled, this, liveOptionsChanged);
    QObject::connect(allowComments_, &QCheckBox::toggled, this, liveOptionsChanged);
    QObject::connect(contentDisclosure_, &QComboBox::currentIndexChanged, this, liveOptionsChanged);
    layout->addWidget(metadataSettings_);
    dual_ = new DualLayoutWidget(this);
    layout->addWidget(dual_);
    accountInfo_->SetDualAccessChangedCallback([this](std::optional<bool> available) {
        dualAccess_ = available; UpdateDualAccess();
    });

    error_ = new QLabel(this);
    error_->setObjectName("connectionError");
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);
    dual_->SetChangedCallback([this] {
        error_->hide();
        RefreshAccountLookup();
        if (contentChanged_) contentChanged_();
    });
    QObject::connect(broadcastType_, &QComboBox::currentIndexChanged, this, [this] { UpdateVisibility(); });
    QObject::connect(method_, &QComboBox::currentIndexChanged, this, [this] {
        UpdateVisibility();
        if (methodChanged_)
            methodChanged_();
    });
    for (auto field : {token_, rapidApiKey_, cookieFile_, deviceId_, installId_})
    {
        QObject::connect(field, &QLineEdit::textChanged, error_, &QWidget::hide);
        QObject::connect(field, &QLineEdit::textChanged, this, [this] { RefreshAccountLookup(); });
    }
    UpdateVisibility();
}

void ConnectionSettingsWidget::UpdateVisibility()
{
    const auto method = static_cast<ConnectionMethod>(method_->currentData().toInt());
    customSettings_->setVisible(method == ConnectionMethod::Custom);
    streamlabsSettings_->setVisible(method == ConnectionMethod::Streamlabs);
    liveStudioSettings_->setVisible(method == ConnectionMethod::TikTokLiveStudio);
    dual_->setVisible(method == ConnectionMethod::TikTokLiveStudio);
    metadataSettings_->setVisible(method != ConnectionMethod::Custom);
    categoryLabel_->setVisible(method == ConnectionMethod::Streamlabs);
    category_->setVisible(method == ConnectionMethod::Streamlabs);
    nativeLiveOptions_->setVisible(method == ConnectionMethod::TikTokLiveStudio);
    mobileOptions_->setVisible(deviceClient_ == "mobile");
    const bool screenCapture = broadcastType_->currentData().toString() == "screen_record";
    orientation_->setVisible(screenCapture);
    orientationLabel_->setVisible(screenCapture);
    error_->hide();
    RefreshAccountLookup();
    UpdateDualAccess();
    updateGeometry();
    if (contentChanged_) contentChanged_();
}

void ConnectionSettingsWidget::Load(const OutputTargetConfig& config)
{
    QSignalBlocker block(method_);
    const int index = method_->findData(static_cast<int>(config.connectionMethod));
    method_->setCurrentIndex(index < 0 ? 0 : index);
    token_->setText(QString::fromStdString(config.streamlabsAccessToken));
    rapidApiKey_->setText(QString::fromStdString(config.rapidApiKey));
    cookieFile_->setText(QString::fromStdString(config.cookieFilePath));
    deviceProfiles_ = config.tiktokDeviceProfiles.is_object() ? config.tiktokDeviceProfiles : nlohmann::json::object();
    deviceClient_ = config.tiktokClient;
    {
        QSignalBlocker blockClient(client_);
        client_->setCurrentIndex(deviceClient_ == "mobile" ? 1 : 0);
    }
    LoadDeviceInputs();
    title_->setText(QString::fromStdString(config.streamlabsTitle));
    category_->SetSelection(QString::fromStdString(config.streamlabsCategory), config.streamlabsCategoryId);
    topics_->SetSelection(QString::fromStdString(config.tiktokTopicId), QString::fromStdString(config.tiktokGameId),
        QString::fromStdString(config.tiktokGameName));
    matureContent_->setChecked(config.streamlabsMatureContent);
    allowGifts_->setChecked(config.tiktokLiveSettings.allowGifts);
    monitorViolations_->setChecked(config.monitorTikTokViolations);
    allowComments_->setChecked(config.tiktokLiveSettings.allowComments);
    const int disclosure = contentDisclosure_->findData(QString::fromStdString(config.tiktokLiveSettings.contentDisclosure));
    contentDisclosure_->setCurrentIndex(disclosure < 0 ? 0 : disclosure);
    dual_->Load(config.dualLayout);
    UpdateVisibility();
}

void ConnectionSettingsWidget::Save(OutputTargetConfig& config) const
{
    config.connectionMethod = static_cast<ConnectionMethod>(method_->currentData().toInt());
    config.streamlabsAccessToken = token_->text().trimmed().toStdString();
    config.rapidApiKey = rapidApiKey_->text().trimmed().toStdString();
    config.cookieFilePath = cookieFile_->text().trimmed().toStdString();
    config.tiktokClient = deviceClient_;
    config.tiktokDeviceProfiles = deviceProfiles_;
    auto& profile = config.tiktokDeviceProfiles[deviceClient_];
    if (!profile.is_object())
        profile = nlohmann::json::object();
    profile["device_id"] = deviceId_->text().trimmed().toStdString();
    profile["install_id"] = installId_->text().trimmed().toStdString();
    if (deviceClient_ == "mobile") {
        profile["broadcast_type"] = broadcastType_->currentData().toString().toStdString();
        profile["orientation"] = orientation_->currentData().toString().toStdString();
    }
    config.tiktokTopicId = topics_->TopicId().toStdString();
    config.tiktokGameId = config.tiktokTopicId == "5" ? topics_->GameId().toStdString() : "";
    config.tiktokGameName = config.tiktokTopicId == "5" ? topics_->GameName().toStdString() : "";
    config.streamlabsTitle = title_->text().toStdString();
    config.streamlabsCategory = category_->Name().toStdString();
    config.streamlabsCategoryId = category_->SelectedId();
    config.streamlabsMatureContent = matureContent_->isChecked();
    config.tiktokLiveSettings = {allowGifts_->isChecked(), allowComments_->isChecked(), contentDisclosure_->currentData().toString().toStdString()};
    config.monitorTikTokViolations = monitorViolations_->isChecked();
    dual_->Save(config.dualLayout);
}

bool ConnectionSettingsWidget::Validate()
{
    OutputTargetConfig config;
    Save(config);
    auto error = ValidateConnectionSettings(config);
    if (!error && config.connectionMethod == ConnectionMethod::TikTokLiveStudio && topics_->RequiresGame() && !topics_->HasValidGame())
        error = "Error.TikTokGame";
    if (!error) {
        error_->hide();
        return dual_->Validate(config);
    }
    const QString key = QString::fromUtf8(error);
    if (key == QString::fromUtf8(ValidateDualLayout(config))) { error_->hide(); return dual_->Validate(config); }
    error_->setText(obs_module_text(error));
    error_->show();
    QWidget* field = method_;
    if (key == "Error.StreamlabsToken") field = token_;
    else if (key == "Error.RapidApiKey") field = rapidApiKey_;
    else if (key.startsWith("Error.CookieFile")) field = cookieFile_;
    else if (key == "Error.TikTokDeviceIds") field = installId_->text().isEmpty() ? installId_ : deviceId_;
    else if (key == "Error.TikTokTopic" || key == "Error.TikTokGame")
        field = topics_->findChild<QComboBox*>(key == "Error.TikTokTopic" ? "tiktokTopic" : "tiktokGame");
    else if (key == "Error.ContentDisclosure") field = contentDisclosure_;
    else if (key == "Error.MobileOrientation") field = orientation_;
    else if (key == "Error.MobileBroadcast" || key == "Dual.Error.CameraOnly") field = broadcastType_;
    RevealSettingsError(field, error_);
    return false;
}

void ConnectionSettingsWidget::SetMethodChangedCallback(std::function<void()> callback)
{
    methodChanged_ = std::move(callback);
}

void ConnectionSettingsWidget::RefreshAccountLookup()
{
    if (!accountInfo_) return;
    OutputTargetConfig config;
    Save(config);
    accountInfo_->SetConfig(config);
    category_->SetContext(config.connectionMethod == ConnectionMethod::Streamlabs, config.streamlabsAccessToken);
    topics_->SetContext(config.connectionMethod == ConnectionMethod::TikTokLiveStudio, QString::fromStdString(config.cookieFilePath),
        deviceClient_ == "mobile" && broadcastType_->currentData().toString() == "video");
}

void ConnectionSettingsWidget::SetContentChangedCallback(std::function<void()> callback)
{
    contentChanged_ = callback;
    accountInfo_->SetContentChangedCallback(callback);
    topics_->SetContentChangedCallback(callback);
    category_->SetContentChangedCallback(std::move(callback));
}

bool ConnectionSettingsWidget::UsesDualLayout() const
{
    return static_cast<ConnectionMethod>(method_->currentData().toInt()) == ConnectionMethod::TikTokLiveStudio && dual_->Enabled();
}
void ConnectionSettingsWidget::UpdateDualAccess()
{
    const bool camera = deviceClient_ != "mobile" || broadcastType_->currentData().toString() == "video";
    dual_->SetAccess(dualAccess_, camera);
}
void ConnectionSettingsWidget::SetLayoutPreview(std::function<void(const LayoutVideoConfig&)> callback) { dual_->preview = std::move(callback); }

void ConnectionSettingsWidget::SaveDeviceInputs()
{
    auto& profile = deviceProfiles_[deviceClient_];
    if (!profile.is_object())
        profile = nlohmann::json::object();
    profile["device_id"] = deviceId_->text().trimmed().toStdString();
    profile["install_id"] = installId_->text().trimmed().toStdString();
    if (deviceClient_ == "mobile") {
        profile["broadcast_type"] = broadcastType_->currentData().toString().toStdString();
        profile["orientation"] = orientation_->currentData().toString().toStdString();
    }
}

void ConnectionSettingsWidget::LoadDeviceInputs()
{
    const auto placeholder = obs_module_text("Connection.AutomaticDevice");
    deviceId_->setPlaceholderText(placeholder);
    installId_->setPlaceholderText(placeholder);
    auto profile = GetJsonField<nlohmann::json>(deviceProfiles_, deviceClient_.c_str()).value_or(nlohmann::json::object());
    {
        QSignalBlocker block(broadcastType_);
        broadcastType_->setCurrentIndex(GetJsonField<std::string>(profile, "broadcast_type").value_or("video") == "screen_record" ? 1 : 0);
        orientation_->setCurrentIndex(GetJsonField<std::string>(profile, "orientation").value_or("1") == "2" ? 1 : 0);
    }
    deviceId_->setText(QString::fromStdString(GetJsonField<std::string>(profile, "device_id").value_or("")));
    installId_->setText(QString::fromStdString(GetJsonField<std::string>(profile, "install_id").value_or("")));
}
