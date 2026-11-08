#pragma once
#include "tiktok-game-info.h"
#include <QWidget>
#include <QTimer>
#include <future>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;

class TikTokTopicWidget : public QWidget {
public:
    explicit TikTokTopicWidget(TikTokGameLookup lookup = LookupTikTokGames, QWidget *parent = nullptr);
    ~TikTokTopicWidget() override;
    void SetContext(bool active, const QString &cookieFile, bool mobileCamera);
    void SetSelection(const QString &topic, const QString &gameId, const QString &gameName);
    QString TopicId() const;
    QString GameId() const;
    QString GameName() const;
    bool RequiresGame() const;
    bool HasValidGame() const;
    void SetContentChangedCallback(std::function<void()> callback) { contentChanged_ = std::move(callback); }
private:
    void UpdateVisibility();
    void Refresh();
    void StartLookup();
    void Poll();
    TikTokGameLookup lookup_;
    std::function<void()> contentChanged_;
    QComboBox *topic_, *game_;
    QWidget *gameRow_;
    QLabel *gameLabel_, *status_;
    QPushButton *refresh_;
    QString cookieFile_, savedId_, savedName_;
    bool active_ = false, mobileCamera_ = false, loaded_ = false, pending_ = false;
    QTimer debounce_, poll_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<TikTokGameResult> result_;
    uint64_t generation_ = 0, runningGeneration_ = 0;
};
