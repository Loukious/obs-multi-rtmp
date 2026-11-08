#include "tiktok-safety.h"
#include "tiktok-native/live_client.hpp"
#include "tiktok-native/request_cancellation.hpp"
#include "tiktok-native/domain_routing.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
QSet<QDialog*> windows;
QSet<TikTokSafetyMonitor*> monitors;
QSet<TikTokSafetyMonitor*> finishingMonitors;
QSet<QString> submitted;
QSet<QString> inFlight;
QString Receipt(const TikTokAppealState& state)
{
    return state.context.userId + ':' + state.route.kind + ':' +
        (state.route.submissionId.isEmpty() ? state.route.id : state.route.submissionId);
}
QLabel* Label(const QString& text, QWidget* parent)
{
    auto label = new QLabel(text, parent); label->setWordWrap(true); label->setTextFormat(Qt::PlainText); return label;
}
class SafetyDialog : public QDialog {
    TikTokSafetyContext context_;
    TikTokSafetyBackend backend_;
    TikTokAppealState state_;
    QListWidget* records_;
    QPlainTextEdit *details_, *explanation_;
    QComboBox* reasons_;
    QLabel *heading_, *eligibility_, *status_, *count_;
    QPushButton *refresh_, *load_, *submit_, *official_, *close_;
    QTimer poll_;
    std::shared_ptr<std::atomic_bool> cancelled_ = std::make_shared<std::atomic_bool>(false);
    std::future<TikTokAppealState> work_;
    std::future<TikTokSafetySnapshot> listWork_;
    bool busy_ = false, needsRefresh_ = true, submitting_ = false;
    QString pendingReceipt_;

    void Controls()
    {
        const bool general = state_.route.kind == "general";
        const bool other = reasons_->currentData().toString() == TikTokAppealReasons().back().second;
        const auto text = explanation_->toPlainText().trimmed();
        reasons_->setVisible(general); explanation_->setVisible(general && other); count_->setVisible(general && other);
        count_->setText(QString("%1 / 140 characters").arg(text.size()));
        submit_->setEnabled(!busy_ && !needsRefresh_ && state_.canSubmit && !submitted.contains(Receipt(state_)) && !inFlight.contains(Receipt(state_)) &&
            (!general || (!reasons_->currentData().toString().isEmpty() && (!other || (!text.isEmpty() && text.size() <= 140)))));
        official_->setEnabled(!busy_ && !state_.route.officialUrl.isEmpty());
        refresh_->setEnabled(!busy_); load_->setEnabled(!busy_ && records_->currentRow() >= 0);
        reasons_->setEnabled(!busy_); explanation_->setEnabled(!busy_); records_->setEnabled(!busy_); close_->setEnabled(!busy_);
    }
    void Fill(const QVector<QJsonObject>& records)
    {
        QSignalBlocker block(records_); records_->clear();
        for (const auto& record : records) {
            auto item = new QListWidgetItem(TikTokViolationSummary(record).replace('\n', " — "), records_);
            item->setData(Qt::UserRole, QJsonDocument(record).toJson(QJsonDocument::Compact));
        }
        if (records_->count()) records_->setCurrentRow(0);
    }
    void RefreshList()
    {
        if (busy_) return;
        busy_ = true; needsRefresh_ = true; status_->setText("Checking active LIVE violations…"); Controls();
        const auto context = context_; const auto backend = backend_; const auto cancelled = cancelled_;
        listWork_ = std::async(std::launch::async, [context, backend, cancelled] {
            TikTokRequestCancellation scope(cancelled.get());
            return backend.check(context);
        });
        poll_.start(50);
    }
    void Load()
    {
        if (busy_ || !records_->currentItem()) return;
        state_ = {}; needsRefresh_ = true;
        const auto record = QJsonDocument::fromJson(records_->currentItem()->data(Qt::UserRole).toByteArray()).object();
        heading_->setText(context_.label + " — LIVE violation"); details_->setPlainText(TikTokViolationSummary(record));
        StartWork(false, record);
    }
    void StartWork(bool submit, QJsonObject record = {})
    {
        if (busy_) return;
        if (submit) {
            const auto key = Receipt(state_);
            if (submitted.contains(key) || inFlight.contains(key)) { Controls(); return; }
            pendingReceipt_ = key; inFlight.insert(key);
        }
        busy_ = true; submitting_ = submit;
        status_->setText(submit ? "Submitting appeal…" : "Checking appeal details and eligibility…"); Controls();
        const auto context = context_; const auto backend = backend_; const auto cancelled = cancelled_;
        const auto reviewed = state_; const auto reason = reasons_->currentData().toString(), explanation = explanation_->toPlainText();
        work_ = std::async(std::launch::async, [=] {
            TikTokRequestCancellation scope(cancelled.get());
            return submit ? backend.submit(reviewed, reason, explanation) : backend.load(context, record);
        });
        poll_.start(50);
    }
    void Poll()
    {
        try {
            if (listWork_.valid()) {
                if (listWork_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
                const auto result = listWork_.get(); busy_ = false; poll_.stop();
                if (!result.error.isEmpty()) { status_->setText(result.error); Controls(); return; }
                context_ = result.context; Fill(result.records); state_ = {};
                status_->setText(records_->count() ? "Active violations loaded." : "No active LIVE violations reported.");
                if (!records_->count()) { details_->clear(); eligibility_->clear(); }
                Controls(); if (records_->count()) Load(); return;
            }
            if (!work_.valid() || work_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            auto result = work_.get(); busy_ = false; poll_.stop();
            inFlight.remove(pendingReceipt_); pendingReceipt_.clear();
            if (!result.error.isEmpty()) {
                needsRefresh_ = true;
                status_->setText(result.error + (submitting_ ? "\nSubmission was not confirmed. Refresh status before trying again." : ""));
                Controls(); return;
            }
            state_ = std::move(result); context_ = state_.context; needsRefresh_ = false;
            if (state_.submitted) submitted.insert(Receipt(state_));
            const auto title = state_.record.value("punish_info").toObject().value("punish_title").toString("LIVE violation");
            heading_->setText(context_.label + " — " + title);
            QStringList text{QString("Status: %1").arg(state_.status), TikTokViolationSummary(state_.record)};
            const auto liveTitle = state_.record.value("live_info").toObject().value("title").toString();
            if (!liveTitle.isEmpty()) text << "LIVE: " + liveTitle;
            for (const auto& reason : state_.detail.value("violation_reason").toArray()) if (reason.isString()) text << reason.toString();
            const auto feature = state_.detail.value("subfeature_name").toString(); if (!feature.isEmpty()) text << "Affected feature: " + feature;
            details_->setPlainText(text.join('\n'));
            eligibility_->setText(state_.reason.isEmpty() ? "Choose a reason and submit to request a review." : state_.reason);
            status_->setText(submitted.contains(Receipt(state_)) ? "Appeal submitted. Refresh details to check TikTok's decision." : "Current details loaded.");
            Controls();
        } catch (const std::exception&) {
            inFlight.remove(pendingReceipt_); pendingReceipt_.clear();
            busy_ = false; poll_.stop(); needsRefresh_ = true;
            status_->setText(submitting_ ? "Submission was not confirmed. Refresh status before trying again." : "Could not check LIVE safety. Try Refresh."); Controls();
        }
    }
public:
    SafetyDialog(TikTokSafetyContext context, const QVector<QJsonObject>& records, TikTokSafetyBackend backend, QWidget* parent)
        : QDialog(parent), context_(std::move(context)), backend_(std::move(backend))
    {
        setObjectName("tiktokSafetyDialog"); setWindowTitle("TikTok LIVE violations and appeals");
        setAttribute(Qt::WA_DeleteOnClose); resize(760, 650); setMinimumSize(560, 480);
        auto layout = new QVBoxLayout(this);
        heading_ = Label(context_.label + " — TikTok LIVE safety", this); layout->addWidget(heading_);
        records_ = new QListWidget(this); records_->setObjectName("tiktokViolationList"); records_->setMaximumHeight(130); layout->addWidget(records_);
        details_ = new QPlainTextEdit(this); details_->setObjectName("tiktokViolationDetails"); details_->setReadOnly(true); layout->addWidget(details_, 1);
        eligibility_ = Label({}, this); layout->addWidget(eligibility_);
        reasons_ = new QComboBox(this); reasons_->setObjectName("tiktokAppealReason"); reasons_->addItem("Select a reason for your appeal", "");
        for (const auto& reason : TikTokAppealReasons()) reasons_->addItem(reason.first, reason.second); layout->addWidget(reasons_);
        explanation_ = new QPlainTextEdit(this); explanation_->setObjectName("tiktokAppealExplanation"); explanation_->setMaximumHeight(90);
        explanation_->setPlaceholderText("Explain your appeal (Other only, up to 140 characters)"); layout->addWidget(explanation_);
        count_ = Label({}, this); layout->addWidget(count_);
        status_ = Label({}, this); status_->setObjectName("tiktokAppealStatus"); layout->addWidget(status_);
        auto row = new QGridLayout;
        refresh_ = new QPushButton("Refresh violations", this); load_ = new QPushButton("Refresh details", this);
        official_ = new QPushButton("Open official details", this); submit_ = new QPushButton("Submit appeal", this); close_ = new QPushButton("Close", this);
        submit_->setObjectName("submitTikTokAppeal"); load_->setObjectName("refreshTikTokAppeal");
        int position = 0;
        for (auto button : {refresh_, load_, official_, submit_, close_}) { button->setAutoDefault(false); row->addWidget(button, position / 3, position % 3); ++position; }
        layout->addLayout(row);
        QObject::connect(&poll_, &QTimer::timeout, this, [this] { Poll(); });
        QObject::connect(refresh_, &QPushButton::clicked, this, [this] { RefreshList(); });
        QObject::connect(load_, &QPushButton::clicked, this, [this] { Load(); });
        QObject::connect(submit_, &QPushButton::clicked, this, [this] { if (submit_->isEnabled()) StartWork(true); });
        QObject::connect(close_, &QPushButton::clicked, this, [this] { reject(); });
        QObject::connect(official_, &QPushButton::clicked, this, [this] { if (official_->isEnabled()) QDesktopServices::openUrl(state_.route.officialUrl); });
        QObject::connect(reasons_, &QComboBox::currentIndexChanged, this, [this] { Controls(); });
        QObject::connect(explanation_, &QPlainTextEdit::textChanged, this, [this] { Controls(); });
        QObject::connect(records_, &QListWidget::currentRowChanged, this, [this] { Load(); });
        Fill(records); Controls();
        QTimer::singleShot(0, this, [this] { if (records_->count()) Load(); else RefreshList(); });
    }
    ~SafetyDialog() override {
        *cancelled_ = true;
        if (work_.valid()) work_.wait(); if (listWork_.valid()) listWork_.wait();
        inFlight.remove(pendingReceipt_);
        windows.remove(this);
    }
    void reject() override { if (!busy_) QDialog::reject(); }
    void closeEvent(QCloseEvent* event) override { if (busy_) event->ignore(); else QDialog::closeEvent(event); }
};
}

TikTokSafetyContext MakeTikTokSafetyContext(const OutputTargetConfig& config)
{
    TikTokSafetyContext context; context.cookiePath = QString::fromStdString(config.cookieFilePath); context.label = QString::fromStdString(config.name);
    auto& account = context.account; account.rapidapi_key = QString::fromStdString(config.rapidApiKey); account.mobile = config.tiktokClient == "mobile";
    const auto profiles = QJsonDocument::fromJson(QByteArray::fromStdString(config.tiktokDeviceProfiles.dump())).object();
    account.device_profile = profiles.value(QString::fromStdString(config.tiktokClient)).toObject(); context.pcProfile = profiles.value("pc").toObject();
    account.device_id = account.device_profile.value("device_id").toString(); account.install_id = account.device_profile.value("install_id").toString();
    account.live_studio_version = account.device_profile.value("live_studio_version").toString("1.27.0");
    QString error; account.cookie_jar = read_tiktok_cookie_file(context.cookiePath, &error);
    context.cookieIdentity = TikTokSafetyCookieIdentity(account.cookie_jar);
    return context;
}

QDialog* ShowTikTokSafety(TikTokSafetyContext context, const QVector<QJsonObject>& records, TikTokSafetyBackend backend, QWidget* parent)
{
    // Settings is modal. A sibling owned by OBS's main window is blocked by
    // it, even if raised; use the launching window as the dialog's owner.
    if (!parent) parent = QApplication::activeModalWidget();
    if (!parent) parent = static_cast<QWidget*>(obs_frontend_get_main_window());
    auto dialog = new SafetyDialog(std::move(context), records, std::move(backend), parent ? parent->window() : nullptr);
    windows.insert(dialog);
    dialog->show(); QApplication::alert(dialog, 0); return dialog;
}
void OpenTikTokSafety(const OutputTargetConfig& config, QWidget* parent)
{
    auto dialog = ShowTikTokSafety(MakeTikTokSafetyContext(config), {}, {}, parent);
    dialog->raise();
    dialog->activateWindow();
}
void ShutdownTikTokSafety()
{
    for (auto monitor : monitors.values()) monitor->Stop();
    for (auto monitor : finishingMonitors.values()) delete monitor;
    for (auto window : windows.values()) delete window;
    submitted.clear(); inFlight.clear();
}
void FinishTikTokSafety(std::unique_ptr<TikTokSafetyMonitor> monitor)
{
    if (!monitor) return;
    auto pending = monitor.get();
    if (pending->Finish([pending] { pending->deleteLater(); })) {
        finishingMonitors.insert(pending);
        monitor.release();
    }
}

TikTokSafetyMonitor::TikTokSafetyMonitor(TikTokSafetyContext context, TikTokSafetyBackend backend, Notify notify)
    : context_(std::move(context)), backend_(std::move(backend)), notify_(std::move(notify)), cancelled_(std::make_shared<std::atomic_bool>(false))
{
    monitors.insert(this);
    if (!notify_) notify_ = [](const auto& context, const auto& records) { ShowTikTokSafety(context, records); };
    schedule_.setSingleShot(true);
    QObject::connect(&schedule_, &QTimer::timeout, this, [this] { CheckNow(); });
    QObject::connect(&poll_, &QTimer::timeout, this, [this] { Poll(); });
}
TikTokSafetyMonitor::~TikTokSafetyMonitor() { Stop(); monitors.remove(this); finishingMonitors.remove(this); }
void TikTokSafetyMonitor::Start() { if (!stopped_ && !finishing_) schedule_.start(0); }
void TikTokSafetyMonitor::Stop()
{
    stopped_ = true; *cancelled_ = true; schedule_.stop(); poll_.stop(); complete_ = {};
    if (result_.valid()) result_.wait();
}
bool TikTokSafetyMonitor::Finish(std::function<void()> complete)
{
    if (stopped_ || finishing_) return false;
    finishing_ = true; complete_ = std::move(complete); schedule_.stop();
    // A request already in flight may predate the disconnect. Consume its
    // result, then check once more without holding up OBS's Stop operation.
    finalCheckPending_ = result_.valid();
    if (!finalCheckPending_) CheckNow();
    return true;
}
void TikTokSafetyMonitor::CheckNow()
{
    if (stopped_ || result_.valid()) return;
    schedule_.stop();
    const auto context = context_; const auto check = backend_.check; const auto cancelled = cancelled_;
    result_ = std::async(std::launch::async, [=] { TikTokRequestCancellation scope(cancelled.get()); return check(context); });
    poll_.start(50);
}
void TikTokSafetyMonitor::Poll()
{
    if (!result_.valid() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    poll_.stop();
    try {
        const auto result = result_.get();
        if (!result.error.isEmpty()) {
            if (lastError_ != result.error) blog(LOG_WARNING, "[obs-multi-rtmp] TikTok violation checks unavailable: %s", result.error.toUtf8().constData());
            lastError_ = result.error;
        } else {
            context_ = result.context; lastError_.clear(); QVector<QJsonObject> unseen;
            for (const auto& record : result.records) {
                auto key = TikTokViolationKey(record);
                if (key.isEmpty()) key = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
                if (!seen_.contains(key)) { seen_.insert(key); unseen.append(record); }
            }
            if (!unseen.isEmpty()) notify_(context_, unseen);
        }
    } catch (const std::exception&) { lastError_ = "LIVE safety check failed."; }
    if (stopped_) return;
    if (finishing_) {
        if (finalCheckPending_) { finalCheckPending_ = false; CheckNow(); }
        else {
            stopped_ = true;
            auto complete = std::move(complete_);
            if (complete) complete();
        }
        return;
    }
    schedule_.start(!lastError_.isEmpty() || QApplication::applicationState() != Qt::ApplicationActive ? 60000 : 15000);
}
