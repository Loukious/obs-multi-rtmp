#include "streamlabs-category-widget.h"
#include <obs-module.h>
#include <QComboBox>
#include <QCompleter>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QVBoxLayout>

StreamlabsCategoryWidget::StreamlabsCategoryWidget(StreamlabsCategoryLookup lookup, QWidget *parent)
    : QWidget(parent), lookup_(std::move(lookup))
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    combo_ = new QComboBox(this);
    combo_->setObjectName("streamlabsCategory");
    combo_->setEditable(true);
    combo_->setInsertPolicy(QComboBox::NoInsert);
    combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo_->setMinimumContentsLength(15);
    combo_->setMaxVisibleItems(10);
    combo_->lineEdit()->setPlaceholderText(obs_module_text("Category.Search"));
    combo_->setToolTip(obs_module_text("Category.SearchLimit"));
    // The dropdown keeps Other and the last results. Autocomplete has its own
    // model so QLineEdit cannot pop up those rows during a pending HTTP search.
    suggestions_ = new QStringListModel(this);
    combo_->setCompleter(new QCompleter(suggestions_, combo_));
    combo_->completer()->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    combo_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    combo_->addItem("Other", QString());
    combo_->setCurrentIndex(-1);
    combo_->installEventFilter(this);
    combo_->lineEdit()->installEventFilter(this);
    combo_->completer()->popup()->installEventFilter(this);
    layout->addWidget(combo_);
    status_ = new QLabel(this);
    status_->setObjectName("streamlabsCategoryStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    // Loading status must not resize the editor underneath a completion popup.
    auto statusPolicy = status_->sizePolicy();
    statusPolicy.setRetainSizeWhenHidden(true);
    status_->setSizePolicy(statusPolicy);
    status_->setMinimumHeight(status_->fontMetrics().height());
    status_->hide();
    layout->addWidget(status_);
    debounce_.setSingleShot(true);
    QObject::connect(combo_->lineEdit(), &QLineEdit::textEdited, this, [this] {
        selectedId_.reset();
        selectedName_.clear();
        QueueSearch(true);
    });
    QObject::connect(combo_, &QComboBox::activated, this, [this](int index) { SelectCurrent(index); });
    QObject::connect(&debounce_, &QTimer::timeout, this, [this] { pending_ = true; StartSearch(); });
    QObject::connect(&poll_, &QTimer::timeout, this, [this] { Poll(); });
}

StreamlabsCategoryWidget::~StreamlabsCategoryWidget()
{
    if (cancelled_) *cancelled_ = true;
    if (result_.valid()) result_.wait();
}

QString StreamlabsCategoryWidget::Name() const { return combo_->currentText(); }
std::optional<std::string> StreamlabsCategoryWidget::SelectedId() const
{
    return Name() == selectedName_ ? selectedId_ : std::nullopt;
}

bool StreamlabsCategoryWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
        showSuggestions_ = false;
    return QWidget::eventFilter(watched, event);
}

void StreamlabsCategoryWidget::SetContext(bool active, const std::string &token)
{
    if (active_ == active && token_ == token) return;
    active_ = active;
    token_ = token;
    QueueSearch(false);
}

void StreamlabsCategoryWidget::SetSelection(const QString &name, const std::optional<std::string> &id)
{
    if (Name() == name && SelectedId() == id) return;
    selectedName_ = name;
    selectedId_ = id;
    {
        QSignalBlocker block(combo_);
        combo_->setEditText(name);
    }
    QueueSearch(false);
}

void StreamlabsCategoryWidget::Populate(const QVector<StreamlabsCategory> &categories)
{
    const auto name = Name();
    const int cursor = combo_->lineEdit()->cursorPosition();
    const int selectionStart = combo_->lineEdit()->selectionStart();
    const int selectionLength = static_cast<int>(combo_->lineEdit()->selectedText().size());
    QSignalBlocker block(combo_);
    QSignalBlocker blockEdit(combo_->lineEdit());
    combo_->clear();
    for (const auto &entry : categories)
        if (combo_->findText(entry.name, Qt::MatchExactly) < 0) combo_->addItem(entry.name, entry.id);
    if (combo_->findText("Other", Qt::MatchExactly) < 0) combo_->addItem("Other", QString());
    if (selectedId_ && name == selectedName_ && combo_->findText(name, Qt::MatchExactly) < 0)
        combo_->addItem(name, QString::fromStdString(*selectedId_));
    combo_->setCurrentIndex(combo_->findText(name, Qt::MatchExactly));
    combo_->setEditText(name);
    combo_->lineEdit()->setCursorPosition(cursor);
    if (selectionStart >= 0) combo_->lineEdit()->setSelection(selectionStart, selectionLength);
}

void StreamlabsCategoryWidget::QueueSearch(bool showSuggestions)
{
    ++generation_;
    pending_ = false;
    debounce_.stop();
    if (cancelled_) *cancelled_ = true;
    showSuggestions_ = showSuggestions;
    combo_->hidePopup();
    combo_->completer()->popup()->hide();
    suggestions_->setStringList({});
    // Do not clear/repopulate the combo from textEdited: Qt is still handling
    // that keystroke and may immediately open its completer after this slot.
    if (!showSuggestions) Populate({});
    status_->hide();
    if (active_ && !token_.empty() && !Name().trimmed().isEmpty() && Name() != "Other" && !SelectedId()) {
        status_->setText(obs_module_text("Category.Searching"));
        status_->show();
        debounce_.start(400);
    }
    if (contentChanged_) contentChanged_();
}

void StreamlabsCategoryWidget::SelectCurrent(int index)
{
    if (index < 0) return;
    selectedName_ = combo_->itemText(index);
    selectedId_ = combo_->itemData(index).toString().toStdString();
    ++generation_;
    pending_ = false;
    debounce_.stop();
    if (cancelled_) *cancelled_ = true;
    showSuggestions_ = false;
    combo_->completer()->popup()->hide();
    suggestions_->setStringList({});
    status_->hide();
    if (contentChanged_) contentChanged_();
}

void StreamlabsCategoryWidget::StartSearch()
{
    if (!pending_ || result_.valid()) return;
    pending_ = false;
    runningGeneration_ = generation_;
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = cancelled_;
    const auto token = token_;
    const auto query = LimitStreamlabsCategoryQuery(Name());
    const auto lookup = lookup_;
    result_ = std::async(std::launch::async, [lookup, token, query, cancelled] {
        try { return lookup(token, query, *cancelled); }
        catch (...) { return StreamlabsCategoryResult{{}, "Category.Unavailable"}; }
    });
    poll_.start(50);
}

void StreamlabsCategoryWidget::Poll()
{
    if (!result_.valid() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto result = result_.get();
    poll_.stop();
    if (runningGeneration_ == generation_ && !*cancelled_) {
        if (result.errorKey) {
            status_->setText(obs_module_text(result.errorKey));
            status_->show();
        } else {
            combo_->hidePopup();
            combo_->completer()->popup()->hide();
            Populate(result.categories);
            const int exact = combo_->findText(Name(), Qt::MatchExactly);
            if (exact >= 0) {
                selectedName_ = Name();
                selectedId_ = combo_->itemData(exact).toString().toStdString();
            }
            const bool hasSuggestions = std::any_of(result.categories.begin(), result.categories.end(),
                [](const auto &category) { return !category.id.isEmpty(); });
            if (hasSuggestions) {
                // Preserve the dropdown's row order for QComboBox activation.
                QStringList names;
                for (int i = 0; i < combo_->count(); ++i) names.append(combo_->itemText(i));
                suggestions_->setStringList(names);
                status_->hide();
            } else {
                status_->setText(obs_module_text("Category.NoResults"));
                status_->show();
            }
        }
        if (contentChanged_) contentChanged_();
        const auto generation = generation_;
        // Run after the parent's queued resize/layout callback above.
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == generation_ && active_ && showSuggestions_ && combo_->lineEdit()->hasFocus() &&
                isVisible() && suggestions_->rowCount() > 0)
                combo_->completer()->complete();
        });
    }
    StartSearch();
}
