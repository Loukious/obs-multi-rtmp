#include "tiktok-safety.h"
#include "tiktok-native/request_cancellation.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QDir>
#include <iostream>
#include <stdexcept>

QString text(const char* key) { return QString::fromUtf8(key); }
QString translated_or(const char*, const QString& fallback) { return fallback; }
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Until(const std::function<bool()>& condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 5000) { QApplication::processEvents(); QThread::msleep(5); }
    Check(condition(), "Safety UI operation timed out");
}
QJsonObject Record(const QString& id = "200")
{
    return {{"violation_id_str", id}, {"live_info", QJsonObject{{"title", "Test LIVE"}}},
        {"punish_info", QJsonObject{{"punish_record_id_str", id}, {"punish_title", "LIVE visibility restriction"},
        {"punish_reason", "Community Guidelines"}, {"appeal_status", 1},
        {"detail_url", "https://livecenter.tiktok.com/punish_detail?record_id=" + id}}}};
}
void MonitorFixtures()
{
    TikTokSafetyContext context; context.userId = "123"; context.label = "@fixture";
    TikTokSafetyBackend backend;
    int notifications = 0, records = 0;
    std::atomic_int generation{0}, calls{0};
    backend.check = [&](TikTokSafetyContext context) {
        ++calls; const int version = generation;
        TikTokSafetySnapshot snapshot; snapshot.context = context;
        if (version == 2) snapshot.error = "Fixture safety service unavailable";
        else { snapshot.records = {Record()}; if (version >= 3) snapshot.records.append(Record("201")); }
        return snapshot;
    };
    TikTokSafetyMonitor monitor(context, backend, [&](const auto&, const auto& newRecords) { ++notifications; records += newRecords.size(); });
    monitor.Start(); Until([&] { return calls == 1 && !monitor.Checking(); });
    Check(notifications == 1 && records == 1, "First active violation triggers one notification");
    monitor.CheckNow(); Until([&] { return calls == 2 && !monitor.Checking(); });
    Check(notifications == 1, "Repeated active-list results do not repeat the popup");
    generation = 2; monitor.CheckNow(); Until([&] { return calls == 3 && !monitor.Checking(); });
    Check(notifications == 1, "Polling failure is not presented as a violation");
    generation = 3; monitor.CheckNow(); Until([&] { return calls == 4 && !monitor.Checking(); });
    Check(notifications == 2 && records == 2, "A second violation notifies only the new record, including after a polling error");
    monitor.Stop(); monitor.CheckNow(); QApplication::processEvents(); Check(calls == 4, "Stopped streams do not keep polling");
    std::atomic_bool started{false};
    backend.check = [&](TikTokSafetyContext context) {
        started = true; while (!tiktok_cancel_requested()) QThread::msleep(2);
        return TikTokSafetySnapshot{context, {}, "cancelled"};
    };
    TikTokSafetyMonitor pending(context, backend, [&](const auto&, const auto&) { throw std::runtime_error("Cancelled check notified"); });
    pending.Start(); Until([&] { return started.load(); }); pending.Stop();
    std::cout << "PASS: nonoverlapping checks, duplicate suppression, error recovery and cancellation\n";

    calls = 0; notifications = 0; records = 0;
    std::atomic_bool release{false};
    backend.check = [&](TikTokSafetyContext context) {
        const int call = ++calls;
        if (call == 2) while (!release && !tiktok_cancel_requested()) QThread::msleep(2);
        QVector<QJsonObject> found{Record()};
        if (call >= 2) found.append(Record("201"));
        if (call >= 3) found.append(Record("202"));
        return TikTokSafetySnapshot{context, found, {}};
    };
    auto draining = std::make_unique<TikTokSafetyMonitor>(context, backend,
        [&](const auto&, const auto& found) { ++notifications; records += found.size(); });
    QPointer<TikTokSafetyMonitor> retained = draining.get();
    draining->Start(); Until([&] { return calls == 1 && !draining->Checking(); });
    draining->CheckNow(); Until([&] { return calls == 2; });
    FinishTikTokSafety(std::move(draining));
    Check(retained && calls == 2, "Disconnect returns while an existing check is still running");
    release = true;
    Until([&] { return calls == 3 && (!retained || !retained->Checking()); });
    Check(notifications == 3 && records == 3, "Disconnect consumes pending violations, then checks once more with duplicate suppression");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    Check(!retained, "Completed disconnect monitor releases its workers and storage");

    calls = 0; release = false;
    backend.check = [&](TikTokSafetyContext context) {
        ++calls; while (!release && !tiktok_cancel_requested()) QThread::msleep(2);
        return TikTokSafetySnapshot{context, {}, "cancelled"};
    };
    draining = std::make_unique<TikTokSafetyMonitor>(context, backend,
        [&](const auto&, const auto&) { throw std::runtime_error("Shutdown must not open a popup"); });
    retained = draining.get();
    FinishTikTokSafety(std::move(draining)); Until([&] { return calls == 1; });
    ShutdownTikTokSafety(); Check(!retained && calls == 1, "OBS shutdown cancels and releases an unfinished disconnect check");
    draining = std::make_unique<TikTokSafetyMonitor>(context, backend);
    retained = draining.get(); ShutdownTikTokSafety();
    FinishTikTokSafety(std::move(draining));
    Check(!retained && calls == 1, "Releasing a session after shutdown destroys its stopped monitor immediately");
    std::cout << "PASS: disconnect checks retain pending violations without blocking Stop and cancel safely on shutdown\n";
}

void DialogFixtures(const QString& screenshotDirectory)
{
    TikTokSafetyContext context; context.userId = "123"; context.label = "@fixture";
    TikTokSafetyBackend backend;
    std::atomic_int loads{0}, writes{0};
    std::atomic_bool release{false}, fail{false};
    backend.load = [&](TikTokSafetyContext context, const QJsonObject& record) {
        ++loads; TikTokAppealState state; state.context = context; state.record = record;
        state.route = TikTokViolationRoute(record); state.route.submissionId = state.route.id;
        state.canSubmit = true; state.status = "Not appealed"; return state;
    };
    backend.submit = [&](const TikTokAppealState& reviewed, const QString&, const QString&) {
        ++writes; while (!release && !tiktok_cancel_requested()) QThread::msleep(2);
        auto result = reviewed; result.canSubmit = false;
        if (fail) result.error = "Fixture connection timeout";
        else result.submitted = true;
        return result;
    };
    backend.check = [](TikTokSafetyContext context) { return TikTokSafetySnapshot{context, {Record()}, {}}; };
    QPointer<QDialog> dialog = ShowTikTokSafety(context, {Record()}, backend);
    auto status = dialog->findChild<QLabel*>("tiktokAppealStatus");
    auto submit = dialog->findChild<QPushButton*>("submitTikTokAppeal");
    auto refresh = dialog->findChild<QPushButton*>("refreshTikTokAppeal");
    auto reason = dialog->findChild<QComboBox*>("tiktokAppealReason");
    auto explanation = dialog->findChild<QPlainTextEdit*>("tiktokAppealExplanation");
    Until([&] { return loads == 1 && refresh->isEnabled(); });
    Check(!dialog->isModal() && writes == 0 && !submit->isEnabled(), "Popup is modeless and opening details cannot submit an appeal");
    reason->setCurrentIndex(reason->count() - 1); explanation->setPlainText(QString::fromUtf8("😀").repeated(71));
    Check(!submit->isEnabled(), "Explanation limit counts UTF-16 units like the original app");
    explanation->setPlainText("Please review this restriction."); Check(submit->isEnabled(), "Explicit valid reason and explanation enable Submit appeal");
    if (!screenshotDirectory.isEmpty()) {
        QDir().mkpath(screenshotDirectory); QApplication::processEvents();
        Check(dialog->grab().save(screenshotDirectory + "/violation-appeal.png"), "Save native appeal popup screenshot");
    }
    int uiTicks = 0; QTimer heartbeat; QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++uiTicks; }); heartbeat.start(5);
    submit->click(); submit->click(); Until([&] { return writes == 1 && uiTicks >= 4; });
    dialog->close(); Check(dialog && dialog->isVisible() && !submit->isEnabled(), "In-flight submission cannot be double-clicked or closed; UI keeps processing events");
    QPointer<QDialog> second = ShowTikTokSafety(context, {Record()}, backend);
    auto secondReason = second->findChild<QComboBox*>("tiktokAppealReason");
    auto secondSubmit = second->findChild<QPushButton*>("submitTikTokAppeal");
    Until([&] { return loads == 2 && second->findChild<QPushButton*>("refreshTikTokAppeal")->isEnabled(); });
    secondReason->setCurrentIndex(1); secondSubmit->click(); Check(writes == 1 && !secondSubmit->isEnabled(), "Two popups cannot submit the same appeal concurrently");
    release = true; Until([&] { return status->text().contains("Appeal submitted"); });
    Check(!submit->isEnabled(), "Successful submission disables duplicate writes");
    refresh->click(); Until([&] { return loads == 3 && refresh->isEnabled(); });
    Check(!submit->isEnabled(), "Local receipt prevents repeats even if TikTok returns stale eligibility");
    ShutdownTikTokSafety(); Check(!dialog && !second, "Shutdown releases popup workers and widgets");

    fail = true; release = true;
    dialog = ShowTikTokSafety(context, {Record()}, backend);
    reason = dialog->findChild<QComboBox*>("tiktokAppealReason"); submit = dialog->findChild<QPushButton*>("submitTikTokAppeal");
    refresh = dialog->findChild<QPushButton*>("refreshTikTokAppeal"); status = dialog->findChild<QLabel*>("tiktokAppealStatus");
    Until([&] { return loads == 4 && refresh->isEnabled(); }); reason->setCurrentIndex(1); submit->click();
    Until([&] { return status->text().contains("not confirmed"); });
    Check(writes == 2 && !submit->isEnabled(), "Ambiguous failures show the real error and require a status refresh, with no retry");
    ShutdownTikTokSafety();
    std::cout << "PASS: native popup, explicit appeal action, UTF-16 validation, responsive workers and duplicate-write prevention\n";
}

void ModalOwnerFixtures()
{
    QWidget mainWindow;
    mainWindow.show();
    TikTokSafetyContext context; context.userId = "123";
    TikTokSafetyBackend backend;
    backend.load = [](TikTokSafetyContext context, const QJsonObject& record) {
        TikTokAppealState state; state.context = context; state.record = record;
        return state;
    };
    for (auto modality : {Qt::ApplicationModal, Qt::WindowModal}) {
        auto settings = std::make_unique<QDialog>(&mainWindow);
        settings->setWindowModality(modality);
        auto launcher = new QWidget(settings.get());
        settings->show();
        Until([&] { return QApplication::activeModalWidget() == settings.get(); });
        QPointer<QDialog> manual = ShowTikTokSafety(context, {Record()}, backend, launcher);
        Until([&] { return manual->findChild<QPushButton*>("refreshTikTokAppeal")->isEnabled(); });
        Check(manual->parentWidget() == settings.get() && !manual->isModal(),
            "Manual violation window belongs to the launching settings window without adding modality");
        QPointer<QDialog> automatic = ShowTikTokSafety(context, {Record()}, backend);
        Until([&] { return automatic->findChild<QPushButton*>("refreshTikTokAppeal")->isEnabled(); });
        Check(automatic->parentWidget() == settings.get() && !automatic->isModal(),
            "Automatic popup opened during modal settings is also owned by those settings");
        settings.reset();
        Check(!manual && !automatic, "Closing settings releases its safety windows and workers");
    }
    std::cout << "PASS: manual and automatic violations remain inside the active settings modal hierarchy\n";
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try { MonitorFixtures(); DialogFixtures(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString()); ModalOwnerFixtures(); return 0; }
    catch (const std::exception& error) { ShutdownTikTokSafety(); std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
