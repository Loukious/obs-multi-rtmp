#pragma once
#include "streamlabs-info.h"
#include <QWidget>
#include <QTimer>
#include <future>
#include <memory>

class QComboBox;
class QLabel;
class QStringListModel;

class StreamlabsCategoryWidget : public QWidget {
public:
    explicit StreamlabsCategoryWidget(StreamlabsCategoryLookup lookup = SearchStreamlabsCategories, QWidget *parent = nullptr);
    ~StreamlabsCategoryWidget() override;
    void SetContext(bool active, const std::string &token);
    void SetSelection(const QString &name, const std::optional<std::string> &id);
    QString Name() const;
    std::optional<std::string> SelectedId() const;
    void SetContentChangedCallback(std::function<void()> callback) { contentChanged_ = std::move(callback); }
private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void QueueSearch(bool showSuggestions);
    void StartSearch();
    void Poll();
    void Populate(const QVector<StreamlabsCategory> &categories);
    void SelectCurrent(int index);
    StreamlabsCategoryLookup lookup_;
    std::function<void()> contentChanged_;
    QComboBox *combo_;
    QStringListModel *suggestions_;
    QLabel *status_;
    std::string token_;
    QString selectedName_;
    std::optional<std::string> selectedId_;
    bool active_ = false, pending_ = false, showSuggestions_ = false;
    QTimer debounce_, poll_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<StreamlabsCategoryResult> result_;
    uint64_t generation_ = 0, runningGeneration_ = 0;
};
