#include "connection-settings.h"
#include "connection-login.h"
#include "account-info-widget.h"
#include "streamlabs-category-widget.h"
#include "tiktok-topic-widget.h"
#include "dual-layout-widget.h"
#include "tiktok-native/rapidapi_quota.hpp"
#include <QProgressBar>
#include <QCheckBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QAbstractItemView>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QFileDialog>
#include <QPixmap>
#include <iostream>
#include <map>
#include <stdexcept>
#include <QElapsedTimer>
#include <QThread>

// Keep UI tests entirely offline, including automatic account lookups.
void OpenTikTokSafety(const OutputTargetConfig&, QWidget*) {}
static QString qrCookiePath, detectedToken;
bool LoginTikTokWithQr(QWidget*, OutputTargetConfig& config) {
    if (qrCookiePath.isEmpty()) return false;
    config.cookieFilePath = qrCookiePath.toStdString();
    config.tiktokDeviceProfiles["pc"] = {{"device_id", "123"}, {"install_id", "456"}};
    return true;
}
QString DetectStreamlabsDesktopAccount(QWidget*) { return detectedToken; }
AccountInfoResult LookupAccountInfo(const OutputTargetConfig &, const std::atomic_bool &)
{
    return {"fixture_account", nullptr, "approved", true, {}, true};
}

StreamlabsCategoryResult SearchStreamlabsCategories(const std::string &, const QString &, const std::atomic_bool &)
{
    return {{{"League of Legends", "123"}, {"Fortnite", "456"}, {"Other", ""}}};
}

TikTokGameResult LookupTikTokGames(const QString &, const std::atomic_bool &)
{
    return {{{"101", "League of Legends"}, {"202", "Fortnite"}, {"303", "Mobile Legends: Bang Bang"}}};
}

static std::map<std::string, std::string> translations;
extern "C" const char* obs_module_text(const char* key)
{
    const auto found = translations.find(key);
    return found == translations.end() ? key : found->second.c_str();
}

static void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static OutputTargetConfigPtr ParseTarget(nlohmann::json target)
{
    auto parsed = DeserializeMultiOutputConfig(nlohmann::json{{"targets", {target}}}.dump());
    Check(parsed.has_value() && parsed->targets.size() == 1, "Target must load");
    return parsed->targets.front();
}

static void Until(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 4000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    Check(condition(), "Asynchronous account lookup timed out");
}

static void CheckAutomaticAndroidProfile()
{
    std::atomic_int calls{0};
    std::atomic_bool started{false};
    ConnectionSettingsWidget widget(new QWidget, nullptr, [&](const OutputTargetConfig& config, const std::atomic_bool& cancelled) {
        ++calls;
        AccountInfoResult result{"mobile_account", nullptr, {}, {}, {}, true};
        if (config.tiktokClient != "mobile") return result;
        if (config.rapidApiKey == "old") {
            started = true;
            while (!cancelled) QThread::msleep(2);
        }
        result.deviceProfile = config.tiktokDeviceProfiles.value("mobile", nlohmann::json::object());
        result.deviceProfile.update({{"device_id", "7654321098765432101"}, {"install_id", "7654321098765432102"},
            {"identifiers", {{"android_id", "abcdef0123456789"}}}, {"fingerprint", {{"model", "Pixel 7"}}},
            {"registration", {{"ssid", "fixture"}}}});
        return result;
    });
    OutputTargetConfig config;
    config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.tiktokClient = "mobile"; config.rapidApiKey = "fixture"; config.cookieFilePath = "fixture.json";
    config.tiktokDeviceProfiles = {{"pc", {{"device_id", "11"}, {"install_id", "12"}}},
        {"mobile", {{"broadcast_type", "screen_record"}, {"orientation", "2"}}}};
    widget.Load(config);
    auto device = widget.findChild<QLineEdit*>("tiktokDeviceId");
    Until([&] { return device->text() == "7654321098765432101"; });
    OutputTargetConfig saved; widget.Save(saved);
    Check(saved.tiktokDeviceProfiles["mobile"]["identifiers"]["android_id"] == "abcdef0123456789" &&
        saved.tiktokDeviceProfiles["mobile"]["registration"]["ssid"] == "fixture" &&
        saved.tiktokDeviceProfiles["mobile"]["broadcast_type"] == "screen_record" &&
        saved.tiktokDeviceProfiles["pc"] == config.tiktokDeviceProfiles["pc"], "Generated profile fills inputs and saves all metadata without changing desktop or mobile options");
    const auto before = calls.load();
    widget.Load(saved);
    QElapsedTimer settle; settle.start();
    while (settle.elapsed() < 750) { QApplication::processEvents(); QThread::msleep(5); }
    Check(calls == before, "Accepting generated IDs does not start an account refresh loop");
    config.rapidApiKey = "old"; widget.Load(config);
    Until([&] { return started.load(); });
    widget.findChild<QComboBox*>("tiktokClient")->setCurrentIndex(0);
    Until([&] { return widget.findChild<QLineEdit*>("tiktokAccountUsername")->text() == "mobile_account"; });
    Check(device->text() == "11", "A stale Android registration cannot overwrite the newly selected client");
    widget.Save(saved);
    Check(!saved.tiktokDeviceProfiles["mobile"].contains("registration"), "Cancelled profile result is discarded");
}

static void CheckAccounts()
{
    std::atomic_bool started{false}, release{false};
    std::atomic_int calls{0};
    std::optional<bool> access = true, reportedAccess;
    AccountInfoWidget widget([&](const OutputTargetConfig &config, const std::atomic_bool &) {
        ++calls;
        if (config.streamlabsAccessToken == "old-token") {
            started = true;
            // Deliberately return after cancellation to exercise stale replies.
            while (!release) QThread::msleep(2);
            return AccountInfoResult{"old_account"};
        }
        if (config.streamlabsAccessToken == "bad-token") return AccountInfoResult{{}, "Account.InvalidToken"};
        if (config.streamlabsAccessToken == "status-only") return AccountInfoResult{{}, nullptr, "pending", false};
        return AccountInfoResult{config.connectionMethod == ConnectionMethod::Streamlabs ? "new_account" : "cookie_account",
            nullptr, "pending", false, {}, access};
    });
    widget.SetDualAccessChangedCallback([&](std::optional<bool> value) { reportedAccess = value; });
    auto username = widget.findChild<QLineEdit*>("tiktokAccountUsername");
    auto status = widget.findChild<QLabel*>("tiktokAccountStatus");
    auto refresh = widget.findChild<QPushButton*>("refreshTikTokAccount");
    auto applicationStatus = widget.findChild<QLineEdit*>("streamlabsApplicationStatus");
    auto canGoLive = widget.findChild<QLineEdit*>("streamlabsCanGoLive");
    OutputTargetConfig config;
    config.connectionMethod = ConnectionMethod::Streamlabs;
    widget.SetConfig(config);
    Check(!refresh->isEnabled(), "No account lookup for blank token");
    config.streamlabsAccessToken = "old-token";
    widget.SetConfig(config);
    Until([&] { return started.load(); });
    config.streamlabsAccessToken = "new-token";
    widget.SetConfig(config);
    release = true;
    Until([&] { return username->text() == "new_account"; });
    Check(applicationStatus->text() == "pending" && canGoLive->text() == obs_module_text("Account.No"),
        "Streamlabs shows status and an explicit false live-access flag");
    Check(username->isReadOnly() && status->textFormat() == Qt::PlainText, "Account fields are read-only plain text");
    const auto before = calls.load();
    widget.SetConfig(config);
    Check(calls == before, "Reloading unchanged settings preserves the account result");
    config.streamlabsAccessToken = "bad-token";
    widget.SetConfig(config);
    Check(username->text() != "new_account", "Changing token immediately clears the old account");
    Check(canGoLive->text() == obs_module_text("Account.Unknown"), "Old live-access flag is cleared on token changes");
    Until([&] { return status->text() == obs_module_text("Account.InvalidToken"); });
    config.streamlabsAccessToken = "status-only";
    widget.SetConfig(config);
    Until([&] { return applicationStatus->text() == "pending"; });
    Check(username->text() == obs_module_text("Account.Unknown") && canGoLive->text() == obs_module_text("Account.No"),
        "Status and live access remain visible when the username is missing");
    config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.rapidApiKey = "dummy-key";
    config.cookieFilePath = "dummy-cookies.json";
    widget.SetConfig(config);
    Until([&] { return username->text() == "cookie_account"; });
    auto canDual = widget.findChild<QLineEdit*>("canDualLayout");
    Check(canDual->isVisibleTo(&widget) && canDual->text() == obs_module_text("Account.Yes") && reportedAccess == true,
        "Can Dual Layout is shown and reported even before dual mode is enabled");
    Check(!applicationStatus->isVisibleTo(&widget) && !canGoLive->isVisibleTo(&widget), "Streamlabs access fields stay hidden for cookie methods");
    const auto beforeRefresh = calls.load();
    access = false;
    refresh->click();
    Check(!reportedAccess.has_value() && canDual->text() == obs_module_text("Account.Unknown"), "Refresh clears stale dual access while pending");
    Until([&] { return calls > beforeRefresh && username->text() == "cookie_account"; });
    Check(reportedAccess == false && canDual->text() == obs_module_text("Account.No"), "Account denial is distinct from an unknown access result");
    config.connectionMethod = ConnectionMethod::Custom;
    widget.SetConfig(config);
    Check(widget.isHidden() && !refresh->isEnabled(), "Custom connections do not attempt a TikTok account lookup");
}

static void CheckCategories()
{
    const QString longName = "A category name that is longer than twenty-five characters";
    std::atomic_int calls{0};
    std::atomic_bool oldStarted{false}, releaseOld{false};
    StreamlabsCategoryWidget widget([&](const std::string &, const QString &query, const std::atomic_bool &) {
        ++calls;
        Check(query.toUcs4().size() <= 25, "UI search requests must never exceed 25 characters");
        if (query == "old") {
            oldStarted = true;
            while (!releaseOld) QThread::msleep(2);
            return StreamlabsCategoryResult{{{"Old result", "old-id"}}};
        }
        return StreamlabsCategoryResult{{{longName, "exact-id"}, {"Other", ""}}};
    });
    auto combo = widget.findChild<QComboBox*>("streamlabsCategory");
    auto type = [&](const QString &text) {
        combo->setEditText(text);
        QMetaObject::invokeMethod(combo->lineEdit(), "textEdited", Q_ARG(QString, text));
    };
    widget.SetContext(true, "dummy-token");
    Check(combo->isEditable() && combo->findText("Other") >= 0, "Searchable dropdown always offers Other");
    type("old");
    Until([&] { return oldStarted.load(); });
    type(longName);
    releaseOld = true;
    Until([&] { return combo->findText(longName) >= 0; });
    Check(combo->findText("Old result") < 0, "Late responses cannot replace newer categories");
    Check(combo->currentText() == longName, "Search clamping must not shorten the displayed category name");
    const auto suggestion = combo->completer()->completionModel()->index(0, 0);
    QMetaObject::invokeMethod(combo->completer(), "activated", Q_ARG(QModelIndex, suggestion));
    Check(widget.SelectedId() == std::optional<std::string>("exact-id"), "Choosing a category retains its exact API ID");
    widget.SetContext(false, "dummy-token");
    widget.SetContext(true, "dummy-token");
    Check(widget.Name() == longName && widget.SelectedId() == std::optional<std::string>("exact-id"), "Switching methods retains the full selection");
    combo->setCurrentIndex(combo->findText("Other"));
    QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, combo->currentIndex()));
    Check(widget.SelectedId().has_value() && widget.SelectedId()->empty(), "Other is a deliberate empty category ID");
    type("typed replacement");
    Check(!widget.SelectedId(), "Typing invalidates the previous selected ID");
    widget.SetContext(false, "");
}

static void Key(QWidget *target, int key, const QString &text = {}, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(target, &release);
    QApplication::processEvents();
}

static void CheckCategoryPopups(const QString &screenshotDir)
{
    std::atomic_bool started{false}, release{false};
    StreamlabsCategoryWidget widget([&](const std::string &, const QString &query, const std::atomic_bool &cancelled) {
        started = true;
        while (!release && !cancelled) QThread::msleep(2);
        if (query == "x") return StreamlabsCategoryResult{{}, "Category.Unavailable"};
        if (query == "z") return StreamlabsCategoryResult{{{"Other", ""}}};
        return StreamlabsCategoryResult{{{"League of Legends", "123"}, {"League of Legends: Wild Rift", "456"}, {"Other", ""}}};
    });
    widget.resize(440, 90);
    widget.move(150, 150);
    widget.SetContentChangedCallback([&] { QTimer::singleShot(0, &widget, [&] { widget.adjustSize(); }); });
    widget.SetContext(true, "dummy-token");
    widget.show();
    widget.activateWindow();
    auto combo = widget.findChild<QComboBox*>("streamlabsCategory");
    auto edit = combo->lineEdit();
    auto popup = combo->completer()->popup();
    auto status = widget.findChild<QLabel*>("streamlabsCategoryStatus");
    edit->setFocus();
    QApplication::processEvents();
    Key(edit, Qt::Key_L, "l");
    if (!screenshotDir.isEmpty() && popup->isVisible()) {
        QDir().mkpath(screenshotDir);
        popup->grab().save(screenshotDir + "/unexpected-pending-popup.png");
    }
    Check(!popup->isVisible(), "Typing must not open an Other-only popup while the Streamlabs search is pending");
    Until([&] { return started.load(); });
    Check(!popup->isVisible() && combo->currentText() == "l", "In-flight searches keep the typed text without a placeholder popup");
    Key(edit, Qt::Key_E, "e");
    edit->setSelection(0, 1);
    release = true;
    Until([&] { return popup->isVisible(); });
    Check(combo->currentText() == "le" && edit->selectedText() == "l", "Async results preserve query text, cursor and selection");
    Check(!combo->view()->isVisible(), "Only the suggestions popup is open, not the combo dropdown as well");
    Check(popup->mapToGlobal(QPoint(0, 0)).y() >= edit->mapToGlobal(QPoint(0, edit->height())).y() - 2,
        "Suggestions stay below the input when the screen has space");
    release = false;
    started = false;
    edit->deselect();
    edit->setCursorPosition(edit->text().size());
    Key(popup, Qt::Key_G, "g");
    Check(widget.Name() == "leg" && !popup->isVisible(), "Typing through an open popup closes it for the next search without changing the query");
    Until([&] { return started.load(); });
    if (!screenshotDir.isEmpty()) widget.grab().save(screenshotDir + "/streamlabs-search-pending.png");
    release = true;
    Until([&] { return popup->isVisible(); });
    if (!screenshotDir.isEmpty()) {
        QDir().mkpath(screenshotDir);
        widget.grab().save(screenshotDir + "/streamlabs-search-ready.png");
        popup->grab().save(screenshotDir + "/streamlabs-suggestions.png");
    }
    Key(popup, Qt::Key_Down);
    const auto choice = popup->currentIndex().data().toString();
    Check(choice.startsWith("League"), "Keyboard navigation highlights a fetched suggestion");
    Key(popup, Qt::Key_Return);
    Check(widget.Name() == choice && widget.SelectedId() == std::optional<std::string>(choice == "League of Legends" ? "123" : "456"),
        "Keyboard suggestion selection commits the correct category ID");
    Check(!popup->isVisible(), "Selecting a suggestion closes its popup");

    release = false;
    started = false;
    edit->setFocus();
    Key(edit, Qt::Key_A, {}, Qt::ControlModifier);
    Key(edit, Qt::Key_L, "l");
    Check(!popup->isVisible() && !combo->view()->isVisible(), "Editing a previous selection hides suggestions until new results arrive");
    Until([&] { return started.load(); });
    Key(edit, Qt::Key_Escape);
    release = true;
    Until([&] { return status->isHidden(); });
    Check(!popup->isVisible(), "Escape while a search is pending prevents late results from reopening the popup");

    Key(edit, Qt::Key_A, {}, Qt::ControlModifier);
    Key(edit, Qt::Key_X, "x");
    Until([&] { return status->text() == obs_module_text("Category.Unavailable"); });
    Check(!popup->isVisible() && widget.Name() == "x", "Failed searches preserve input without an Other-only popup");
    Key(edit, Qt::Key_A, {}, Qt::ControlModifier);
    Key(edit, Qt::Key_Z, "z");
    Until([&] { return status->text() == obs_module_text("Category.NoResults"); });
    Check(!popup->isVisible(), "Empty searches report no matches without popping up Other");
    combo->showPopup();
    combo->view()->setCurrentIndex(combo->model()->index(combo->findText("Other"), 0));
    Key(combo->view(), Qt::Key_Return);
    Check(widget.Name() == "Other" && widget.SelectedId().has_value() && widget.SelectedId()->empty(),
        "Other remains selectable from the explicit dropdown");
}

static void CheckNativeTopics()
{
    std::atomic_bool oldStarted{false}, releaseOld{false};
    TikTokTopicWidget widget([&](const QString &path, const std::atomic_bool &cancelled) {
        if (path == "old") {
            oldStarted = true;
            while (!releaseOld && !cancelled) QThread::msleep(2);
            return TikTokGameResult{{{"999", "Old game"}}};
        }
        if (path == "bad") return TikTokGameResult{{}, "Topic.GamesUnavailable"};
        return LookupTikTokGames(path, cancelled);
    });
    auto game = widget.findChild<QComboBox*>("tiktokGame");
    auto status = widget.findChild<QLabel*>("tiktokGamesStatus");
    widget.SetSelection("5", "202", "Old display name");
    widget.SetContext(true, "old", false);
    Until([&] { return oldStarted.load(); });
    Check(!game->isEnabled() && !widget.HasValidGame() && widget.GameId() == "202", "Saved game ID is preserved while its catalogue loads");
    widget.SetContext(true, "new", false);
    releaseOld = true;
    Until([&] { return game->isEnabled(); });
    Check(game->findText("Old game") < 0 && game->currentText() == "Fortnite" && widget.GameId() == "202",
        "Stale catalogue is ignored and saved game restores by ID, not old display name");
    Check(game->completer()->caseSensitivity() == Qt::CaseInsensitive && game->completer()->filterMode() == Qt::MatchContains,
        "Game search matches the original case-insensitive substring completer");
    game->completer()->setCompletionPrefix("legends");
    Check(game->completer()->completionCount() == 2, "Game completer offers all matching substrings");
    widget.SetContext(false, "new", false);
    widget.SetContext(true, "new", false);
    Until([&] { return game->isEnabled(); });
    Check(widget.GameId() == "202", "Game selection survives switching methods");
    widget.SetContext(true, "bad", false);
    Until([&] { return status->text() == obs_module_text("Topic.GamesUnavailable"); });
    Check(!game->isEnabled() && !widget.HasValidGame(), "Failed catalogue loads cannot validate stale entries");
    widget.SetContext(true, "new", false);
    Until([&] { return game->isEnabled(); });
    Check(widget.GameId() == "202", "Retry restores the saved selection");
    widget.SetSelection("5", "missing", "Removed game");
    Check(!widget.HasValidGame(), "Removed game IDs must be reselected");
    widget.resize(440, 140);
    widget.move(150, 150);
    widget.show();
    widget.activateWindow();
    game->lineEdit()->setFocus();
    QApplication::processEvents();
    Key(game->lineEdit(), Qt::Key_L, "l");
    Key(game->lineEdit(), Qt::Key_E, "e");
    Key(game->lineEdit(), Qt::Key_G, "g");
    auto popup = game->completer()->popup();
    Check(popup->isVisible() && game->completer()->completionCount() == 2,
        "Native game typing uses the loaded catalogue without an Other placeholder");
    widget.findChild<QPushButton*>("refreshTikTokGames")->click();
    Check(!popup->isVisible() && !game->view()->isVisible() && !game->isEnabled(), "Native refresh closes both popups before loading the catalogue");
    Until([&] { return game->isEnabled(); });
    Check(!popup->isVisible(), "Native catalogue refresh does not reopen an unsolicited popup");
}

static void CheckPersistence()
{
    auto custom = ParseTarget({{"id", "custom"}, {"service-param", {{"password", "rtmp-password"}, {"use_auth", true}}}});
    Check(custom->connectionMethod == ConnectionMethod::Custom, "Existing targets default to custom");
    Check(custom->serviceParam["password"] == "rtmp-password", "Custom authentication must remain intact");

    auto legacy = ParseTarget({{"id", "legacy"}, {"streamlabs-token", true},
        {"service-param", {{"password", "legacy-api-token"}, {"use_auth", true}}}});
    Check(legacy->connectionMethod == ConnectionMethod::Streamlabs, "Old checkbox must select Streamlabs");
    Check(legacy->streamlabsAccessToken == "legacy-api-token", "Legacy API token must migrate");
    Check(!legacy->serviceParam.contains("password") && legacy->serviceParam["use_auth"] == false,
        "Legacy API token must no longer be RTMP authentication");

    auto explicitCustom = ParseTarget({{"id", "explicit"}, {"connection-method", "custom"}, {"streamlabs-token", true},
        {"service-param", {{"password", "keep-me"}}}});
    Check(explicitCustom->connectionMethod == ConnectionMethod::Custom && explicitCustom->serviceParam["password"] == "keep-me",
        "Explicit method must take precedence over legacy fields");
    auto unknown = ParseTarget({{"id", "unknown"}, {"connection-method", "future-method"}});
    Check(unknown->connectionMethod == ConnectionMethod::Custom, "Unknown methods must use the default");
    const auto other = ParseTarget({{"id", "other"}, {"streamlabs-category", "Other"}, {"streamlabs-category-id", ""}});
    Check(other->streamlabsCategoryId.has_value() && other->streamlabsCategoryId->empty(), "An explicit Other selection differs from an unresolved legacy category");

    for (auto method : {ConnectionMethod::Custom, ConnectionMethod::Streamlabs, ConnectionMethod::TikTokLiveStudio}) {
        MultiOutputConfig config;
        auto target = std::make_shared<OutputTargetConfig>();
        target->id = "roundtrip";
        target->connectionMethod = method;
        target->protocol = "WHIP";
        target->streamlabsAccessToken = "saved-token";
        target->rapidApiKey = "saved-rapidapi-key";
        target->tiktokClient = "mobile";
        target->tiktokTopicId = "5";
        target->tiktokGameId = "202";
        target->tiktokGameName = "Fortnite";
        target->tiktokDeviceProfiles = {{"pc", {{"device_id", "11"}, {"install_id", "12"}}},
            {"mobile", {{"device_id", "21"}, {"install_id", "22"}, {"fingerprint", {{"model", "sample"}}}}}};
        target->cookieFilePath = "D:/Cookies with spaces/日本語.json";
        target->streamlabsTitle = "Saved title";
        target->streamlabsCategory = "Saved category";
        target->streamlabsCategoryId = "saved-id";
        target->streamlabsMatureContent = true;
        target->tiktokLiveSettings = {false, false, "both"};
        target->serviceParam = {{"server", "https://custom.example/whip"}, {"bearer_token", "custom-secret"}};
        config.targets.push_back(target);
        auto roundtrip = DeserializeMultiOutputConfig(SerializeMultiOutputConfig(config))->targets.front();
        Check(roundtrip->connectionMethod == method && roundtrip->streamlabsAccessToken == target->streamlabsAccessToken &&
            roundtrip->rapidApiKey == target->rapidApiKey && roundtrip->cookieFilePath == target->cookieFilePath,
            "Connection settings must survive serialization");
        Check(roundtrip->streamlabsCategoryId == target->streamlabsCategoryId, "Selected category ID survives serialization");
        Check(!roundtrip->tiktokLiveSettings.allowGifts && !roundtrip->tiktokLiveSettings.allowComments && roundtrip->tiktokLiveSettings.contentDisclosure == "both",
            "Gifts, comments and disclosure survive serialization for all connection methods");
        Check(roundtrip->tiktokTopicId == "5" && roundtrip->tiktokGameId == "202" && roundtrip->tiktokGameName == "Fortnite",
            "Native topic and game IDs and display name survive serialization");
        Check(roundtrip->tiktokClient == "mobile" && roundtrip->tiktokDeviceProfiles == target->tiktokDeviceProfiles,
            "Both device profiles and additional device data must persist");
        Check(roundtrip->serviceParam == target->serviceParam && roundtrip->streamlabsTitle == target->streamlabsTitle &&
            roundtrip->streamlabsCategory == target->streamlabsCategory && roundtrip->streamlabsMatureContent,
            "Hidden method values and metadata must survive serialization");
        Check(std::string(roundtrip->OutputProtocol()) == (method == ConnectionMethod::Custom ? "WHIP" : "RTMP"),
            "TikTok must use RTMP without overwriting the saved custom protocol");
    }
}

static void CheckWidget(const QString& screenshotDir)
{
    auto custom = new QWidget();
    custom->setObjectName("customSettings");
    auto customLayout = new QFormLayout(custom);
    customLayout->addRow("Server URL", new QLineEdit(custom));
    customLayout->addRow("Stream key", new QLineEdit(custom));
    ConnectionSettingsWidget widget(custom);
    widget.resize(580, 300);
    widget.show();
    QApplication::processEvents();
    auto method = widget.findChild<QComboBox*>("connectionMethod");
    auto token = widget.findChild<QLineEdit*>("streamlabsAccessToken");
    auto key = widget.findChild<QLineEdit*>("rapidApiKey");
    auto cookies = widget.findChild<QLineEdit*>("cookieFilePath");
    auto streamlabs = widget.findChild<QWidget*>("streamlabsSettings");
    auto studio = widget.findChild<QWidget*>("liveStudioSettings");
    auto error = widget.findChild<QLabel*>("connectionError");
    Check(method->count() == 3 && method->currentIndex() == 0, "Exactly three methods, custom selected by default");
    Check(custom->isVisible() && !streamlabs->isVisible() && !studio->isVisible(), "Default shows only custom fields");
    Check(widget.Validate(), "Custom method must not require TikTok credentials");
    Check(token->echoMode() == QLineEdit::Password && key->echoMode() == QLineEdit::Password, "Credentials must be masked");

    int changes = 0;
    widget.SetMethodChangedCallback([&] { ++changes; });
    method->setCurrentIndex(1);
    Check(changes == 1 && !custom->isVisible() && streamlabs->isVisible() && !studio->isVisible(), "Streamlabs selection updates the form");
    token->setText("   ");
    Check(!widget.Validate() && error->isVisible(), "Blank token must show a validation error");
    token->setText(" demo-token ");
    Check(widget.Validate() && !error->isVisible(), "Entering a token clears the error");
    OutputTargetConfig saved;
    saved.serviceParam = {{"server", "rtmp://saved-custom"}, {"password", "custom-password"}};
    const auto originalService = saved.serviceParam;
    widget.Save(saved);
    Check(saved.streamlabsAccessToken == "demo-token", "Token is trimmed");

    method->setCurrentIndex(2);
    Check(!custom->isVisible() && !streamlabs->isVisible() && studio->isVisible(), "Studio shows only its fields");
    Check(!widget.Validate(), "Studio requires a RapidAPI key");
    key->setText("demo-rapidapi-key");
    Check(!widget.Validate(), "Studio requires a cookie file");
    QTemporaryDir temp;
    Check(temp.isValid(), "Temporary directory must be created");
    cookies->setText(temp.path());
    Check(!widget.Validate(), "Cookie path cannot be a directory");
    cookies->setText(temp.filePath("missing.json"));
    Check(!widget.Validate(), "Cookie path must exist");
    const auto cookiePath = temp.filePath("cookies with spaces.json");
    QFile cookieFile(cookiePath);
    Check(cookieFile.open(QIODevice::WriteOnly), "Dummy cookie file must be created");
    cookieFile.write("[]");
    cookieFile.close();
    cookies->setText(cookiePath);
    Check(!widget.Validate() && error->text() == obs_module_text("Error.TikTokTopic"), "Native topic must be explicitly selected");
    auto topic = widget.findChild<QComboBox*>("tiktokTopic");
    topic->setCurrentIndex(topic->findData("6"));
    Check(widget.Validate(), "Readable cookie file and key satisfy required fields");
    auto client = widget.findChild<QComboBox*>("tiktokClient");
    auto device = widget.findChild<QLineEdit*>("tiktokDeviceId");
    auto install = widget.findChild<QLineEdit*>("tiktokInstallId");
    device->setText("11");
    Check(!widget.Validate(), "Partial desktop identity is rejected");
    install->setText("12");
    Check(widget.Validate(), "Complete desktop identity accepted");
    client->setCurrentIndex(1);
    Check(device->text().isEmpty() && install->text().isEmpty() && widget.Validate(), "Blank Android IDs enable automatic setup without copying desktop IDs");
    device->setText("21");
    Check(!widget.Validate(), "Partial Android identity is rejected");
    device->setText("21"); install->setText("22");
    Check(widget.Validate(), "Android IDs accepted");
    client->setCurrentIndex(0);
    Check(device->text() == "11" && install->text() == "12", "Desktop IDs restored on switching back");
    client->setCurrentIndex(1);
    Check(device->text() == "21" && install->text() == "22", "Android IDs restored on switching back");
    install->setText("0");
    Check(!widget.Validate(), "Zero device identifiers rejected");
    install->setText("22");
    client->setCurrentIndex(0);

    auto game = widget.findChild<QComboBox*>("tiktokGame");
    auto broadcast = widget.findChild<QComboBox*>("tiktokBroadcastType");
    auto orientation = widget.findChild<QComboBox*>("tiktokOrientation");
    Check(topic->count() == 10 && topic->itemText(0).isEmpty(), "Topic list contains a blank and the original nine topics");
    Check(!game->isVisible(), "Music has no game subcategory");
    topic->setCurrentIndex(topic->findData("5"));
    Check(game->isVisible() && !widget.Validate(), "Desktop Gaming requires a game");
    Until([&] { return game->isEnabled(); });
    game->setEditText("invented game");
    Check(!widget.Validate(), "Unmatched game text cannot be saved as a category ID");
    game->setEditText("fortnite");
    Check(widget.Validate(), "Case-insensitive exact game name is accepted");
    widget.Save(saved);
    Check(saved.tiktokTopicId == "5" && saved.tiktokGameId == "202", "Native topic and game selections save the API IDs");
    game->setCurrentIndex(0);
    client->setCurrentIndex(1);
    Check(!game->isVisible() && !orientation->isVisible() && widget.Validate(), "Mobile camera Gaming does not require a game or orientation");
    broadcast->setCurrentIndex(1);
    Check(game->isVisible() && orientation->isVisible() && !widget.Validate(), "Mobile screen capture Gaming requires a game");
    game->setCurrentIndex(game->findData("303"));
    orientation->setCurrentIndex(1);
    Check(widget.Validate(), "Mobile screen capture accepts selected game and landscape orientation");
    widget.Save(saved);
    Check(saved.tiktokDeviceProfiles["mobile"]["broadcast_type"] == "screen_record" &&
        saved.tiktokDeviceProfiles["mobile"]["orientation"] == "2", "Mobile broadcast options are stored in its device profile");
    topic->setCurrentIndex(topic->findData("6"));
    widget.Save(saved);
    Check(saved.tiktokGameId.empty() && !game->isVisible() && widget.Validate(), "Non-Gaming saves no game ID");
    topic->setCurrentIndex(topic->findData("5"));
    broadcast->setCurrentIndex(0);
    broadcast->setCurrentIndex(1);
    Check(orientation->currentData().toString() == "2" && game->currentData().toString() == "303", "Switching broadcast modes retains game and orientation");
    client->setCurrentIndex(0);
    client->setCurrentIndex(1);
    Check(broadcast->currentData().toString() == "screen_record" && orientation->currentData().toString() == "2", "Mobile options survive client changes");
    client->setCurrentIndex(0);

    // Exercise the real Browse callback with Qt's non-native file dialog.
    cookies->clear();
    QTimer picker;
    QObject::connect(&picker, &QTimer::timeout, [&] {
        for (auto top : QApplication::topLevelWidgets()) {
            if (auto dialog = qobject_cast<QFileDialog*>(top); dialog && dialog->isVisible()) {
                picker.stop();
                dialog->setDirectory(temp.path());
                QTimer::singleShot(500, dialog, [dialog, cookiePath] {
                    dialog->selectFile(cookiePath);
                    if (auto fileName = dialog->findChild<QLineEdit*>("fileNameEdit"))
                        fileName->setText(QDir::toNativeSeparators(cookiePath));
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                });
            }
        }
    });
    picker.start(50);
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::cerr << "FAIL: file picker timed out\n";
        std::exit(1);
    });
    watchdog.start(5000);
    widget.findChild<QPushButton*>("browseCookieFile")->click();
    watchdog.stop();
    Check(cookies->text() == cookiePath, "Browse must populate the selected file path");
    method->setCurrentIndex(0);
    widget.Save(saved);
    Check(saved.connectionMethod == ConnectionMethod::Custom && saved.streamlabsAccessToken == "demo-token" &&
        saved.rapidApiKey == "demo-rapidapi-key" && saved.cookieFilePath == cookiePath.toStdString(), "Switching methods retains credentials");
    Check(saved.serviceParam == originalService, "Connection form must not overwrite custom service settings");
    saved.connectionMethod = ConnectionMethod::Streamlabs;
    const int beforeLoad = changes;
    widget.Load(saved);
    Check(changes == beforeLoad && method->currentIndex() == 1 && streamlabs->isVisible(), "Loading must not emit method changes");

    if (!screenshotDir.isEmpty()) {
        QDir().mkpath(screenshotDir);
        token->clear();
        key->clear();
        cookies->clear();
        for (int i = 0; i < 3; ++i) {
            method->setCurrentIndex(i);
            widget.adjustSize();
            QApplication::processEvents();
            Check(widget.grab().save(screenshotDir + "/method-" + QString::number(i) + ".png"), "Screenshot must save");
        }
        token->setText("demo-token");
        key->setText("demo-rapidapi-key");
        cookies->setText(cookiePath);
        for (int i : {1, 2}) {
            method->setCurrentIndex(i);
            Until([&] { return widget.findChild<QLineEdit*>("tiktokAccountUsername")->text() == "fixture_account"; });
            widget.adjustSize();
            QApplication::processEvents();
            Check(widget.grab().save(screenshotDir + "/account-" + QString::number(i) + ".png"), "Account preview must save");
        }
        method->setCurrentIndex(1);
        auto category = widget.findChild<QComboBox*>("streamlabsCategory");
        auto categoryWidget = static_cast<StreamlabsCategoryWidget*>(category->parentWidget());
        categoryWidget->SetSelection("League", {});
        Until([&] { return category->findText("League of Legends") >= 0; });
        category->setCurrentIndex(category->findText("League of Legends"));
        QMetaObject::invokeMethod(category, "activated", Q_ARG(int, category->currentIndex()));
        widget.Save(saved);
        Check(saved.streamlabsCategory == "League of Legends" && saved.streamlabsCategoryId == std::optional<std::string>("123"),
            "Connection form saves the selected category name and API ID together");
        Until([&] { return widget.findChild<QLineEdit*>("tiktokAccountUsername")->text() == "fixture_account"; });
        widget.adjustSize();
        QApplication::processEvents();
        Check(widget.grab().save(screenshotDir + "/streamlabs-settings.png"), "Streamlabs preview must save");
        category->showPopup();
        QApplication::processEvents();
        Check(category->view()->window()->grab().save(screenshotDir + "/category-options.png"), "Category popup preview must save");
        category->hidePopup();
        method->setCurrentIndex(2);
        topic->setCurrentIndex(topic->findData("5"));
        Until([&] { return game->isEnabled(); });
        game->setCurrentIndex(game->findData("303"));
        for (int mode : {0, 1, 2}) {
            client->setCurrentIndex(mode == 0 ? 0 : 1);
            if (mode != 0) broadcast->setCurrentIndex(mode - 1);
            widget.adjustSize();
            QApplication::processEvents();
            Check(widget.grab().save(screenshotDir + "/native-topic-" + QString::number(mode) + ".png"), "Native Topic/Game preview must save");
        }
        topic->showPopup();
        QApplication::processEvents();
        Check(topic->view()->window()->grab().save(screenshotDir + "/topic-options.png"), "Topic popup preview must save");
        topic->hidePopup();
        game->showPopup();
        QApplication::processEvents();
        Check(game->view()->window()->grab().save(screenshotDir + "/game-options.png"), "Game popup preview must save");
        game->hidePopup();
    }
}

static void CheckDualLayout(const QString& screenshotDir)
{
    DualUiCatalog catalog{{{"portrait-scene", "Portrait scene"}, {"landscape-scene", "Landscape scene"}},
        {{"test_h264", "H.264"}}, {{"ffmpeg_aac", "AAC"}}};
    DualLayoutWidget widget(nullptr, [&] { return catalog; });
    auto combo = [&](const char* name) { return widget.findChild<QComboBox*>(name); };
    auto same = widget.findChild<QCheckBox*>("dualSameAudio");
    Check(same->isChecked() && !widget.Enabled(), "Legacy/default settings use single layout and shared audio");
    Check(!(combo("tiktokLayout")->model()->flags(combo("tiktokLayout")->model()->index(1, 0)) & Qt::ItemIsEnabled), "Dual option disabled until account access is verified");
    widget.SetAccess(true);
    combo("tiktokLayout")->setCurrentIndex(1);
    widget.show(); QApplication::processEvents();
    const auto portraitBox = widget.findChild<QGroupBox*>("portraitVideoSettings");
    const auto landscapeBox = widget.findChild<QGroupBox*>("landscapeVideoSettings");
    Check(portraitBox->y() == landscapeBox->y() && portraitBox->x() < landscapeBox->x(), "Portrait and landscape video settings appear side by side");
    Check(combo("portraitScene")->isVisible() && combo("landscapeScene")->isVisible(), "Both scene selectors shown with shared audio");
    Check(!combo("portraitAudioTrack")->isVisible() && combo("sharedAudioTrack")->isVisible(), "Shared mode hides independent audio fields");
    combo("portraitScene")->setCurrentIndex(1); combo("landscapeScene")->setCurrentIndex(2);
    combo("sharedAudioTrack")->setCurrentIndex(5);
    same->setChecked(false);
    Check(combo("portraitAudioTrack")->currentData().toInt() == 5 && combo("landscapeAudioTrack")->currentData().toInt() == 5,
        "First independent selection clones the user's shared track, without fixed layout assignments");
    combo("portraitAudioTrack")->setCurrentIndex(4); combo("landscapeAudioTrack")->setCurrentIndex(2);
    same->setChecked(true); same->setChecked(false);
    Check(combo("portraitAudioTrack")->currentData().toInt() == 4 && combo("landscapeAudioTrack")->currentData().toInt() == 2,
        "Independent track 5/3 choices survive sharing toggles");
    Check(combo("portraitScene")->isVisible() && combo("landscapeScene")->isVisible(), "Both scenes remain selectable with independent audio");
    auto target = std::make_shared<OutputTargetConfig>();
    target->id = "dual-test"; target->connectionMethod = ConnectionMethod::TikTokLiveStudio;
    target->videoConfig = "old-video"; target->audioConfig = "old-audio";
    widget.Save(target->dualLayout);
    widget.editEncoder = [](const std::string&, nlohmann::json& params, int& bitrate, bool audio) {
        params["fixture-preset"] = audio ? "audio" : "video"; bitrate = audio ? 192 : 4500; return true;
    };
    widget.findChild<QPushButton*>("portraitVideoAdvanced")->click();
    widget.findChild<QPushButton*>("landscapeAudioAdvanced")->click();
    widget.findChild<QSpinBox*>("portraitFPSDivisor")->setValue(2);
    widget.Save(target->dualLayout);
    Check(!ValidateDualLayout(*target), "Arbitrary valid independent audio tracks accepted");
    MultiOutputConfig config; config.targets.push_back(target);
    auto loaded = DeserializeMultiOutputConfig(SerializeMultiOutputConfig(config));
    const auto& copy = *loaded->targets.front();
    Check(copy.dualLayout.portrait.fpsDivisor == 2 && copy.dualLayout.portrait.encoderParams["fixture-preset"] == "video" &&
        copy.dualLayout.landscape.audio.encoderParams["fixture-preset"] == "audio" && copy.dualLayout.landscape.audio.bitrate == 192,
        "Per-layout advanced video/audio properties and frame rate survive serialization");
    Check(copy.dualLayout.portrait.sceneUuid == "portrait-scene" && copy.dualLayout.landscape.sceneUuid == "landscape-scene" &&
        LayoutAudio(copy.dualLayout, true).mixer == 4 && LayoutAudio(copy.dualLayout, false).mixer == 2 &&
        copy.dualLayout.sharedAudio.mixer == 5 && copy.videoConfig == "old-video" && copy.audioConfig == "old-audio",
        "Roundtrip preserves both scenes, all audio settings, and single-mode settings");
    target->dualLayout.landscape.audio.mixer = 4;
    Check(!ValidateDualLayout(*target), "Same track allowed with separate encoders");
    bool previewed = false;
    widget.preview = [&](const LayoutVideoConfig& c) { previewed = c.sceneUuid == "portrait-scene" && c.height > c.width; };
    widget.findChild<QPushButton*>("portraitPreview")->click();
    Check(previewed, "Preview uses the current selected portrait composition");
    if (!screenshotDir.isEmpty()) {
        QDir().mkpath(screenshotDir); widget.adjustSize(); QApplication::processEvents();
        Check(widget.grab().save(screenshotDir + "/dual-independent.png"), "Save independent dual UI screenshot");
        same->setChecked(true); widget.adjustSize(); QApplication::processEvents();
        Check(widget.grab().save(screenshotDir + "/dual-shared.png"), "Save shared dual UI screenshot");
    }
    catalog.scenes[0].name = "Renamed portrait";
    widget.findChild<QPushButton*>("refreshDualSources")->click();
    Check(combo("portraitScene")->currentText() == "Renamed portrait" && combo("landscapeScene")->currentData() == "landscape-scene",
        "Scene rename preserves UUID and the other layout's selection");
    catalog.scenes.removeFirst(); widget.findChild<QPushButton*>("refreshDualSources")->click();
    Check(combo("portraitScene")->currentData() == "portrait-scene" && combo("portraitScene")->currentText().contains(obs_module_text("Dual.Missing")),
        "Removed scene stays explicitly unavailable instead of selecting a different scene");
    widget.SetAccess(false);
    Check(!widget.Enabled() && !(combo("tiktokLayout")->model()->flags(combo("tiktokLayout")->model()->index(1, 0)) & Qt::ItemIsEnabled), "Locked account disables dual and returns to single");
    widget.SetAccess(true, false);
    Check(!(combo("tiktokLayout")->model()->flags(combo("tiktokLayout")->model()->index(1, 0)) & Qt::ItemIsEnabled), "Mobile screen capture cannot enable dual despite account access");
}

static void CheckSingleSceneDualSetup()
{
    DualUiCatalog catalog{{{"input", "scene"}}, {{"test_h264", "H.264"}}, {{"ffmpeg_aac", "AAC"}}};
    catalog.currentSceneId = "input";
    catalog.generated = {{"portrait-output", "portrait-canvas", "input", "scene"},
        {"landscape-output", "landscape-canvas", "input", "scene"}};
    DualLayoutWidget widget(nullptr, [&] { return catalog; });
    widget.SetAccess(true); widget.SetNativeCanvasMode(true);
    DualLayoutConfig fresh; fresh.enabled = true;
    widget.Load(fresh);
    auto portrait = widget.findChild<QComboBox*>("portraitScene");
    auto landscape = widget.findChild<QComboBox*>("landscapeScene");
    Check(portrait->currentText() == "scene" && landscape->currentText() == "scene",
        "A new dual setup defaults both layouts to the user's current scene");
    auto saved = fresh;
    saved.portrait.sceneUuid = "portrait-output"; saved.portrait.canvasUuid = "portrait-canvas";
    saved.landscape.sceneUuid = "landscape-output"; saved.landscape.canvasUuid = "landscape-canvas";
    widget.Load(saved);
    Check(portrait->count() == 2 && landscape->count() == 2 && portrait->currentText() == "scene" && landscape->currentText() == "scene",
        "Reopening prepared dual settings shows only the real scene, not generated output names");
    DualLayoutConfig roundtrip; widget.Save(roundtrip);
    Check(roundtrip.portrait.sceneUuid == "portrait-output" && roundtrip.landscape.sceneUuid == "landscape-output",
        "Unchanged scene selectors reuse the prepared outputs");
    widget.findChild<QSpinBox*>("portraitWidth")->setValue(saved.portrait.width + 2);
    widget.Save(roundtrip);
    Check(roundtrip.portrait.sceneUuid == "input" && roundtrip.portrait.canvasUuid.empty() &&
        roundtrip.landscape.canvasUuid == "landscape-canvas", "Changing a composition rebuilds only that layout from the actual source scene");
}

static void CheckLoginControls()
{
    ConnectionSettingsWidget widget(new QWidget);
    OutputTargetConfig config; config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.tiktokClient = "mobile";
    config.tiktokDeviceProfiles["mobile"] = {{"device_id", "777"}, {"install_id", "888"}};
    widget.Load(config);
    int changed = 0; widget.SetContentChangedCallback([&] { ++changed; });
    QTemporaryDir directory;
    qrCookiePath = directory.filePath("qr-cookies.txt");
    widget.findChild<QPushButton*>("tiktokQrLogin")->click();
    widget.Save(config);
    Check(changed > 0 && config.cookieFilePath == qrCookiePath.toStdString() && config.tiktokClient == "mobile" &&
        config.tiktokDeviceProfiles["mobile"]["device_id"] == "777" && config.tiktokDeviceProfiles["pc"]["device_id"] == "123",
        "QR login fills cookie input and desktop identity without replacing Android identity");
    qrCookiePath.clear();
    const auto before = config.cookieFilePath;
    widget.findChild<QPushButton*>("tiktokQrLogin")->click(); widget.Save(config);
    Check(config.cookieFilePath == before, "Cancelled QR login preserves selected cookie file");
    widget.findChild<QComboBox*>("connectionMethod")->setCurrentIndex(1);
    detectedToken = "fixture-local-token";
    widget.findChild<QPushButton*>("detectStreamlabsAccount")->click(); widget.Save(config);
    Check(config.streamlabsAccessToken == "fixture-local-token" &&
        widget.findChild<QLineEdit*>("streamlabsAccessToken")->echoMode() == QLineEdit::Password,
        "Local detection fills the masked token input");
    detectedToken.clear();
    widget.findChild<QPushButton*>("detectStreamlabsAccount")->click(); widget.Save(config);
    Check(config.streamlabsAccessToken == "fixture-local-token", "Missing or cancelled local detection preserves current token");
}

static void CheckLiveSettings()
{
    for (const auto input : {nlohmann::json::object(), nlohmann::json{{"tiktok-allow-gifts", "false"}, {"tiktok-allow-comments", nullptr}, {"tiktok-content-disclosure", "invalid"}}}) {
        auto target = input; target["id"] = "live-settings-fixture";
        auto config = ParseTarget(target);
        Check(config->tiktokLiveSettings.allowGifts && config->tiktokLiveSettings.allowComments && config->tiktokLiveSettings.contentDisclosure == "off",
            "Legacy and malformed saved options keep the original defaults");
    }
    ConnectionSettingsWidget widget(new QWidget);
    OutputTargetConfig config; config.connectionMethod = ConnectionMethod::TikTokLiveStudio; widget.Load(config);
    auto gifts = widget.findChild<QCheckBox*>("tiktokAllowGifts");
    auto comments = widget.findChild<QCheckBox*>("tiktokAllowComments");
    auto disclosure = widget.findChild<QComboBox*>("tiktokContentDisclosure");
    Check(gifts->isChecked() && comments->isChecked() && disclosure->count() == 4 && disclosure->currentData() == "off", "New LIVE defaults and all four disclosure choices are shown");
    int changed = 0; widget.SetContentChangedCallback([&] { ++changed; });
    gifts->setChecked(false); comments->setChecked(false); disclosure->setCurrentIndex(disclosure->findData("third_party"));
    widget.Save(config);
    Check(changed >= 3 && !config.tiktokLiveSettings.allowGifts && !config.tiktokLiveSettings.allowComments && config.tiktokLiveSettings.contentDisclosure == "third_party",
        "LIVE controls save user selections and notify the main settings of changes");
    widget.findChild<QComboBox*>("tiktokClient")->setCurrentIndex(1);
    widget.findChild<QComboBox*>("connectionMethod")->setCurrentIndex(1);
    Check(widget.findChild<QWidget*>("tiktokLiveOptions")->isHidden(), "Unsupported Streamlabs method does not show native room options");
    widget.findChild<QComboBox*>("connectionMethod")->setCurrentIndex(2); widget.Save(config);
    Check(!config.tiktokLiveSettings.allowGifts && !config.tiktokLiveSettings.allowComments && config.tiktokLiveSettings.contentDisclosure == "third_party",
        "LIVE settings survive switching methods and clients");
}

static void CheckQuotaMeter(const QString& screenshotDir)
{
    SetRapidApiQuotaDirectory({});
    ConnectionSettingsWidget widget(new QWidget);
    OutputTargetConfig config; config.connectionMethod = ConnectionMethod::TikTokLiveStudio;
    config.rapidApiKey = "quota-ui-fixture";
    widget.Load(config); widget.resize(720, 700); widget.show();
    auto bar = widget.findChild<QProgressBar*>("rapidApiQuotaBar");
    Check(bar && bar->isVisible() && bar->value() == 0 && bar->format() == obs_module_text("Account.Unknown"),
        "Native method shows unknown quota before the first response");
    HostedSigningServiceConfig service; service.api_key = QString::fromStdString(config.rapidApiKey);
    int changes = 0; widget.SetContentChangedCallback([&] { ++changes; });
    RecordRapidApiQuota(service, {{"x-quota-limit", "10000"}, {"x-quota-remaining", "1000"}, {"x-quota-reset", "3600"}});
    Until([&] { return bar->value() == 100; });
    Check(bar->styleSheet().contains("#d9534f") && changes == 0, "Ten percent remaining turns red without making settings dirty");
    if (!screenshotDir.isEmpty()) {
        QDir().mkpath(screenshotDir);
        Check(bar->parentWidget()->grab().save(screenshotDir + "/quota-low.png"), "Save quota meter screenshot");
    }
    RecordRapidApiQuota(service, {{"x-quota-limit", "10000"}, {"x-quota-remaining", "9437"}});
    Until([&] { return bar->value() == 943; });
    Check(!bar->styleSheet().contains("#d9534f"), "Recovered quota restores the normal theme color");
    auto key = widget.findChild<QLineEdit*>("rapidApiKey"); key->setText("different-key");
    Check(bar->value() == 0 && bar->format() == obs_module_text("Account.Unknown"), "Changing keys immediately clears another key's quota");
    key->setText(service.api_key);
    Check(bar->value() == 943, "Returning to a known key restores its cached quota");
    widget.findChild<QComboBox*>("tiktokClient")->setCurrentIndex(1);
    Check(bar->isVisible() && bar->value() == 943, "Mobile shares the selected RapidAPI key's quota");
    widget.findChild<QComboBox*>("connectionMethod")->setCurrentIndex(1);
    Check(!bar->isVisible(), "Streamlabs does not display RapidAPI quota");
    widget.findChild<QComboBox*>("connectionMethod")->setCurrentIndex(0);
    Check(!bar->isVisible(), "Custom RTMP does not display RapidAPI quota");
    SetRapidApiQuotaDirectory({});
}

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    try {
        if (argc > 1) {
            QSettings locale(QString::fromLocal8Bit(argv[1]), QSettings::IniFormat);
            for (const auto& key : locale.allKeys())
                translations[key.toStdString()] = locale.value(key).toString().toStdString();
        }
        CheckPersistence();
        CheckLoginControls();
        CheckLiveSettings();
        CheckQuotaMeter(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
        CheckDualLayout(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
        CheckSingleSceneDualSetup();
        CheckAccounts();
        CheckAutomaticAndroidProfile();
        CheckCategories();
        CheckCategoryPopups(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
        CheckNativeTopics();
        CheckWidget(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
        std::cout << "PASS: migration, persistence, protocol selection, visibility, required fields, Browse, and method switching\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
