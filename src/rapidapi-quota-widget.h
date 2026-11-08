#pragma once
#include "tiktok-native/hosted_signing_service.hpp"
#include <QWidget>
#include <QTimer>
class QProgressBar;
class RapidApiQuotaWidget : public QWidget {
public:
    explicit RapidApiQuotaWidget(QWidget* parent = nullptr);
    void SetApiKey(const QString& key);
private:
    void Refresh();
    HostedSigningServiceConfig service_;
    QProgressBar* bar_;
    QTimer timer_;
};
