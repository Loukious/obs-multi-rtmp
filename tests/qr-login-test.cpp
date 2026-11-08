// Exercise the private dialog itself, including its worker and timer callbacks.
// All native HTTP calls use fixtures; no real TikTok or RapidAPI requests run.
#include "../src/connection-login.cpp"
#include "tiktok-native/http_fixture.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <iostream>
#include <mutex>
#include <stdexcept>

extern "C" const char* obs_module_text(const char* key) { return key; }
extern "C" obs_module_t* obs_current_module(void) { return nullptr; }
QString text(const char* key) { return QString::fromUtf8(key); }
QString translated_or(const char*, const QString& fallback) { return fallback; }

namespace {
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Until(const std::function<bool()>& condition)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        QApplication::processEvents(); QThread::msleep(5);
    }
    Check(condition(), "QR dialog operation timed out");
}
QByteArray Json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

struct Backend {
    std::atomic_int generations{0}, generationSignatures{0}, pollSignatures{0}, polls{0};
    std::atomic_int httpStatus{200};
    std::atomic<CURLcode> curlCode{CURLE_OK};
    std::atomic_bool failGeneration{false}, blockPoll{false}, enteredPoll{false};
    std::atomic_bool failAccountLookup{false};
    std::mutex mutex;
    QString status = "new", secret;

    Backend()
    {
        tiktok_http_fixture = [this](const QByteArray&, const QUrl& url, const TikTokHttpHeaders&, const QByteArray& body) -> TikTokHttpFixtureResponse {
            if (url.path() == "/get_domains/v4/") return {200, R"({"data":{"ttnet_dispatch_actions":[{"param":{"strategy_info":{"api.tiktokv.com":"api16-normal-c-alisg.tiktokv.com"}}}]}})"};
            if (url.path() == "/passport/account/info/v2/") {
                if (failAccountLookup) return {503, R"({"message":"Account info temporarily unavailable"})"};
                return {200, R"({"data":{"user_id_str":"123456","username":"qr_ui_user"}})"};
            }
            if (url.path() == "/webcast/game/basic/region_check/") return {200, R"({"status_code":0,"data":{"priority_region":"us"}})"};
            if (url.path() == "/signatures") {
                const auto params = QJsonDocument::fromJson(body).object().value("params").toString();
                const bool generating = !QUrlQuery(params).hasQueryItem("token");
                if (generating) ++generationSignatures; else ++pollSignatures;
                if (!generating && blockPoll) {
                    enteredPoll = true;
                    while (!tiktok_cancel_requested()) QThread::msleep(2);
                    return {0, {}, {}, {}, {}, CURLE_ABORTED_BY_CALLBACK};
                }
                if (!generating || failGeneration) {
                    if (curlCode != CURLE_OK) return {0, {}, {}, {}, {}, curlCode.load()};
                    if (httpStatus != 200) return {httpStatus.load(), R"({"message":"Fixture signer rejection"})"};
                }
                return {200, R"({"x-khronos":"1","x-ladon":"l","x-argus":"a"})"};
            }
            if (url.path() == "/passport/web/get_qrcode/") {
                const auto generation = ++generations;
                return {200, Json({{"data", QJsonObject{{"token", QString("ui-token-%1").arg(generation)},
                    {"qrcode_index_url", QString("https://www.tiktok.com/login?next_url=https%3A%2F%2Fwww.tiktok.com%2Fconfirm%3Ftoken%3Dui-%1").arg(generation)}}}})};
            }
            Check(url.path() == "/passport/web/check_qrconnect/", "UI recovery must only poll QR login");
            ++polls;
            std::lock_guard lock(mutex);
            return {200, Json({{"data", QJsonObject{{"status", status}, {"client_secret", secret}}}}), {},
                status == "confirmed" ? QByteArray(".tiktok.com\tTRUE\t/\tTRUE\t0\tsessionid\tqr-ui-session\n") : QByteArray()};
        };
    }
    ~Backend() { tiktok_http_fixture = {}; }
    void SetStatus(const QString& value, const QString& clientSecret)
    {
        std::lock_guard lock(mutex); status = value; secret = clientSecret;
    }
    TikTokStudioAccountCredentials Account() const
    {
        TikTokStudioAccountCredentials account;
        account.device_id = "701"; account.install_id = "702"; account.rapidapi_key = "qr-ui-fixture-key";
        return account;
    }
};

struct View {
    QrLoginDialog dialog;
    QTimer* poll;
    QLabel* picture;
    QLabel* status;
    QPushButton* regenerate;
    View(Backend& backend, const QString& directory) : dialog(backend.Account(), directory, nullptr)
    {
        poll = dialog.findChild<QTimer*>("qrPollTimer");
        picture = dialog.findChild<QLabel*>("qrPicture");
        status = dialog.findChild<QLabel*>("qrStatus");
        regenerate = dialog.findChild<QPushButton*>("qrRegenerate");
        Check(poll && picture && status && regenerate, "QR controls available");
        dialog.show(); Until([&] { return poll->isActive(); });
        backend.SetStatus("new", dialog.login.client_secret);
    }
    void Poll()
    {
        Check(poll->isActive(), "A QR poll must be scheduled before firing it");
        Check(QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection), "Invoke scheduled QR poll");
    }
};

void RecoveryAndConfirmation()
{
    Backend backend; QTemporaryDir directory; View view(backend, directory.path());
    const auto token = view.dialog.login.token;
    const auto png = view.dialog.login.png;
    backend.curlCode = CURLE_OPERATION_TIMEDOUT;
    for (const int delay : {5000, 10000, 20000}) {
        view.Poll(); Until([&] { return view.poll->isActive(); });
        Check(view.poll->interval() == delay && !view.picture->pixmap().isNull() &&
            view.dialog.login.token == token && view.dialog.login.png == png && backend.generations == 1,
            "Temporary failures retain the code and increase the scheduled delay");
    }
    backend.curlCode = CURLE_OK; backend.SetStatus("scanned", view.dialog.login.client_secret);
    view.Poll(); Until([&] { return view.poll->isActive(); });
    Check(view.poll->interval() == 2500 && view.status->text() == "Login.Confirm" && !view.dialog.login.retryable,
        "Successful polling restores the normal interval and phone confirmation prompt");
    backend.curlCode = CURLE_OPERATION_TIMEDOUT;
    view.Poll(); Until([&] { return view.poll->isActive(); });
    Check(view.poll->interval() == 5000, "A successful poll resets failure backoff");
    backend.curlCode = CURLE_OK; backend.SetStatus("confirmed", view.dialog.login.client_secret);
    view.Poll(); Until([&] { return !view.dialog.isVisible(); });
    Check(view.dialog.result() == QDialog::Accepted && QFile::exists(view.dialog.cookiePath) &&
        backend.generations == 1 && !view.poll->isActive(), "Recovered confirmation saves cookies and stops polling");
    Check(QFileInfo(view.dialog.cookiePath).fileName() == "cookies_qr_ui_user.json",
        "QR dialog looks up the username missing from confirmation and names the JSON cookie file");
    QFile cookies(view.dialog.cookiePath);
    Check(cookies.open(QIODevice::ReadOnly), "Open recovered cookie export");
    const auto saved = cookies.readAll();
    Check(saved.contains("qr-ui-session") && QJsonDocument::fromJson(saved).isArray(), "Recovered session is saved as JSON");
}

void AutomaticRenewal()
{
    Backend backend; QTemporaryDir directory; View view(backend, directory.path());
    for (const auto state : {"expired", "refuse", "error"}) {
        const auto token = view.dialog.login.token, secret = view.dialog.login.client_secret;
        const auto png = view.dialog.login.png;
        const int generations = backend.generations;
        backend.SetStatus(state, secret);
        view.Poll(); Until([&] { return backend.generations > generations && view.poll->isActive(); });
        Check(backend.generations == generations + 1 && view.dialog.login.token != token &&
            view.dialog.login.client_secret != secret && view.dialog.login.png != png && !view.picture->pixmap().isNull(),
            "Invalidated codes automatically renew with a fresh token, secret and image");
        backend.SetStatus("new", view.dialog.login.client_secret);
    }
    const int generations = backend.generations;
    backend.curlCode = CURLE_OPERATION_TIMEDOUT;
    for (const int delay : {5000, 10000, 20000, 30000, 30000, 30000}) {
        view.Poll(); Until([&] { return view.poll->isActive(); });
        Check(view.poll->interval() == delay && backend.generations == generations,
            "Outage retries are bounded before code renewal");
    }
    view.Poll(); Until([&] { return backend.generations > generations && view.poll->isActive(); });
    Check(backend.generations == generations + 1 && view.poll->interval() == 2500,
        "Exhausted polling retries request one fresh code");
    view.dialog.reject();
}

void TerminalFailureAndCancellation()
{
    {
        Backend backend; QTemporaryDir directory; View view(backend, directory.path());
        backend.failAccountLookup = true;
        backend.SetStatus("confirmed", view.dialog.login.client_secret);
        view.Poll(); Until([&] { return !view.dialog.isVisible(); });
        Check(view.dialog.result() == QDialog::Accepted && QFile::exists(view.dialog.cookiePath) &&
            view.dialog.cookiePath.endsWith(".json"), "An unavailable username lookup preserves the confirmed login");
    }
    for (const int status : {401, 403, 429}) {
        Backend backend; QTemporaryDir directory; View view(backend, directory.path());
        backend.httpStatus = status;
        view.Poll(); Until([&] { return view.regenerate->isVisible(); });
        Check(!view.poll->isActive() && backend.generations == 1 && backend.pollSignatures == 1 &&
            view.dialog.cookiePath.isEmpty(), "Key, subscription and quota failures stop without renewing");
        view.dialog.reject();
    }
    {
        Backend backend; QTemporaryDir directory; View view(backend, directory.path());
        backend.curlCode = CURLE_OPERATION_TIMEDOUT;
        view.Poll(); Until([&] { return view.poll->isActive(); });
        const int calls = backend.pollSignatures;
        view.dialog.reject(); QApplication::processEvents();
        Check(!view.poll->isActive() && !view.dialog.isVisible() && backend.pollSignatures == calls &&
            view.dialog.cookiePath.isEmpty(), "Cancelling backoff stops further requests");
    }
    {
        Backend backend; QTemporaryDir directory; View view(backend, directory.path());
        backend.blockPoll = true;
        view.Poll(); Until([&] { return backend.enteredPoll.load(); });
        view.dialog.reject(); Until([&] { return !view.dialog.isVisible(); });
        Check(!view.poll->isActive() && backend.pollSignatures == 1 && view.dialog.cookiePath.isEmpty(),
            "Cancelling an active request waits for worker cancellation without saving cookies");
    }
    {
        Backend backend; QTemporaryDir directory; View view(backend, directory.path());
        backend.curlCode = CURLE_OPERATION_TIMEDOUT;
        for (int retry = 0; retry < 6; ++retry) {
            view.Poll(); Until([&] { return view.poll->isActive(); });
        }
        backend.failGeneration = true;
        view.Poll(); Until([&] { return backend.generationSignatures == 2 && view.regenerate->isVisible(); });
        Check(!view.poll->isActive() && backend.generations == 1 && view.dialog.cookiePath.isEmpty(),
            "A failed renewal stops instead of repeatedly generating during an outage");
        view.dialog.reject();
    }
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        RecoveryAndConfirmation(); AutomaticRenewal(); TerminalFailureAndCancellation();
        std::cout << "PASS: QR dialog recovery, automatic renewal, cookies, terminal errors and cancellation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
