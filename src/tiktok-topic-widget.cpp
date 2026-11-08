#include "tiktok-topic-widget.h"
#include "tiktok-native/tiktok_studio_topics.hpp"
#include <obs-module.h>
#include <QComboBox>
#include <QCompleter>
#include <QAbstractItemView>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

TikTokTopicWidget::TikTokTopicWidget(TikTokGameLookup lookup, QWidget *parent)
    : QWidget(parent), lookup_(std::move(lookup))
{
    auto layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    topic_ = new QComboBox(this);
    topic_->setObjectName("tiktokTopic");
    topic_->addItem(QString(), QString());
    for (const auto &topic : tiktok_studio_topics()) topic_->addItem(topic.name, topic.id);
    layout->addRow(obs_module_text("Topic.Topic"), topic_);
    gameRow_ = new QWidget(this);
    auto gamesLayout = new QVBoxLayout(gameRow_);
    gamesLayout->setContentsMargins(0, 0, 0, 0);
    auto row = new QHBoxLayout;
    game_ = new QComboBox(gameRow_);
    game_->setObjectName("tiktokGame");
    game_->setEditable(true);
    game_->setInsertPolicy(QComboBox::NoInsert);
    game_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    game_->setMinimumContentsLength(20);
    game_->lineEdit()->setPlaceholderText(obs_module_text("Topic.SearchGames"));
    game_->addItem(QString(), QString());
    game_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    game_->completer()->setFilterMode(Qt::MatchContains);
    game_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    refresh_ = new QPushButton(obs_module_text("Account.Refresh"), gameRow_);
    refresh_->setObjectName("refreshTikTokGames");
    refresh_->setAutoDefault(false);
    row->addWidget(game_, 1);
    row->addWidget(refresh_);
    gamesLayout->addLayout(row);
    status_ = new QLabel(gameRow_);
    status_->setObjectName("tiktokGamesStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    gamesLayout->addWidget(status_);
    gameLabel_ = new QLabel(obs_module_text("Topic.Game"), this);
    layout->addRow(gameLabel_, gameRow_);
    QObject::connect(topic_, &QComboBox::currentIndexChanged, this, [this] { UpdateVisibility(); });
    QObject::connect(game_->lineEdit(), &QLineEdit::textEdited, this, [this] { savedId_.clear(); savedName_.clear(); });
    QObject::connect(refresh_, &QPushButton::clicked, this, [this] { Refresh(); });
    debounce_.setSingleShot(true);
    QObject::connect(&debounce_, &QTimer::timeout, this, [this] { pending_ = true; StartLookup(); });
    QObject::connect(&poll_, &QTimer::timeout, this, [this] { Poll(); });
    game_->setEnabled(false);
    UpdateVisibility();
}

TikTokTopicWidget::~TikTokTopicWidget()
{
    if (cancelled_) *cancelled_ = true;
    if (result_.valid()) result_.wait();
}

QString TikTokTopicWidget::TopicId() const { return topic_->currentData().toString(); }
bool TikTokTopicWidget::RequiresGame() const { return tiktok_studio_topic_is_gaming(TopicId()) && !mobileCamera_; }

QString TikTokTopicWidget::GameId() const
{
    if (!loaded_) return savedId_;
    const auto name = game_->currentText().trimmed();
    for (int i = 1; i < game_->count(); ++i)
        if (game_->itemText(i).compare(name, Qt::CaseInsensitive) == 0) return game_->itemData(i).toString();
    return {};
}

QString TikTokTopicWidget::GameName() const { return loaded_ ? game_->currentText().trimmed() : savedName_; }
bool TikTokTopicWidget::HasValidGame() const { return loaded_ && !GameId().isEmpty(); }

void TikTokTopicWidget::SetSelection(const QString &topic, const QString &gameId, const QString &gameName)
{
    const int index = topic_->findData(topic);
    topic_->setCurrentIndex(index < 0 ? 0 : index);
    savedId_ = gameId;
    savedName_ = gameName;
    const int gameIndex = game_->findData(gameId);
    game_->setCurrentIndex(gameIndex < 0 ? 0 : gameIndex);
    if (!loaded_) game_->setEditText(gameName);
    UpdateVisibility();
}

void TikTokTopicWidget::SetContext(bool active, const QString &cookieFile, bool mobileCamera)
{
    const bool changed = active != active_ || cookieFile != cookieFile_;
    active_ = active;
    cookieFile_ = cookieFile;
    mobileCamera_ = mobileCamera;
    setVisible(active);
    UpdateVisibility();
    if (changed) Refresh();
}

void TikTokTopicWidget::UpdateVisibility()
{
    gameLabel_->setVisible(RequiresGame());
    gameRow_->setVisible(RequiresGame());
    updateGeometry();
    if (contentChanged_) contentChanged_();
}

void TikTokTopicWidget::Refresh()
{
    // A catalogue refresh must close both Qt popups before replacing the model.
    game_->hidePopup();
    game_->completer()->popup()->hide();
    savedId_ = GameId();
    savedName_ = GameName();
    loaded_ = false;
    game_->setEnabled(false);
    ++generation_;
    pending_ = false;
    debounce_.stop();
    if (cancelled_) *cancelled_ = true;
    const bool ready = active_ && !cookieFile_.isEmpty();
    refresh_->setEnabled(ready);
    status_->setText(obs_module_text(ready ? "Topic.LoadingGames" : "Topic.ChooseCookies"));
    status_->show();
    if (ready) debounce_.start(400);
    if (contentChanged_) contentChanged_();
}

void TikTokTopicWidget::StartLookup()
{
    if (!pending_ || result_.valid()) return;
    pending_ = false;
    runningGeneration_ = generation_;
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = cancelled_;
    const auto path = cookieFile_;
    const auto lookup = lookup_;
    result_ = std::async(std::launch::async, [path, lookup, cancelled] {
        try { return lookup(path, *cancelled); }
        catch (...) { return TikTokGameResult{{}, "Topic.GamesUnavailable"}; }
    });
    poll_.start(50);
}

void TikTokTopicWidget::Poll()
{
    if (!result_.valid() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto result = result_.get();
    poll_.stop();
    if (runningGeneration_ == generation_ && !*cancelled_) {
        if (result.errorKey || result.games.isEmpty()) {
            status_->setText(obs_module_text(result.errorKey ? result.errorKey : "Topic.GamesUnavailable"));
            status_->show();
        } else {
            QSignalBlocker block(game_);
            game_->clear();
            game_->addItem(QString(), QString());
            for (const auto &game : result.games) game_->addItem(game.name, game.id);
            const int index = game_->findData(savedId_);
            game_->setCurrentIndex(index < 0 ? 0 : index);
            loaded_ = true;
            savedId_.clear();
            savedName_.clear();
            game_->setEnabled(true);
            status_->hide();
        }
        if (contentChanged_) contentChanged_();
    }
    StartLookup();
}
