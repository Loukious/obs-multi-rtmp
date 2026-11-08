#include "rapidapi-quota-widget.h"
#include "tiktok-native/rapidapi_quota.hpp"
#include <obs-module.h>
#include <QDateTime>
#include <QFormLayout>
#include <QLocale>
#include <QProgressBar>
#include <algorithm>

RapidApiQuotaWidget::RapidApiQuotaWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName("rapidApiQuota");
    auto layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    bar_ = new QProgressBar(this);
    bar_->setObjectName("rapidApiQuotaBar");
    bar_->setAccessibleName(obs_module_text("Quota.Label"));
    bar_->setMinimumHeight(24);
    bar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bar_->setTextVisible(true);
    layout->addRow(obs_module_text("Quota.Label"), bar_);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) Refresh(); });
    timer_.start(1000);
    Refresh();
}

void RapidApiQuotaWidget::SetApiKey(const QString& key)
{
    service_.api_key = key.trimmed();
    Refresh();
}

void RapidApiQuotaWidget::Refresh()
{
    const auto quota = ReadRapidApiQuota(service_);
    const QLocale locale;
    bool low = false;
    bar_->setRange(0, 1000);
    if (quota.limit > 0 && quota.remaining >= 0) {
        const auto remaining = std::clamp(quota.remaining, qint64(0), quota.limit);
        const double fraction = double(remaining) / double(quota.limit);
        bar_->setValue(int(fraction * 1000));
        bar_->setFormat(QString::fromUtf8(obs_module_text("Quota.RemainingOf")).arg(locale.toString(remaining), locale.toString(quota.limit)));
        low = fraction <= 0.1;
    } else if (quota.remaining >= 0) {
        bar_->setValue(quota.remaining > 0 ? 1000 : 0);
        bar_->setFormat(QString::fromUtf8(obs_module_text("Quota.Remaining")).arg(locale.toString(quota.remaining)));
        low = quota.remaining == 0;
    } else {
        bar_->setValue(0);
        bar_->setFormat(obs_module_text("Account.Unknown"));
    }
    const QString style = QStringLiteral("QProgressBar { border: 1px solid palette(mid); border-radius: 4px; background: palette(button); text-align: center; color: palette(text); } QProgressBar::chunk { border-radius: 3px; background: %1; }")
        .arg(low ? "#d9534f" : "palette(highlight)");
    if (bar_->styleSheet() != style) bar_->setStyleSheet(style);
    QStringList tips{QString::fromUtf8(obs_module_text("Quota.Passive"))};
    if (quota.has_calendar_reset()) tips << QString::fromUtf8(obs_module_text("Quota.Reset")).arg(
        locale.toString(QDateTime::fromSecsSinceEpoch(quota.reset_epoch_seconds).toLocalTime(), QLocale::ShortFormat));
    if (quota.observed_epoch_seconds > 0) tips << QString::fromUtf8(obs_module_text("Quota.Updated")).arg(
        locale.toString(QDateTime::fromSecsSinceEpoch(quota.observed_epoch_seconds).toLocalTime(), QLocale::ShortFormat));
    else tips << QString::fromUtf8(obs_module_text("Quota.NoUsage"));
    bar_->setToolTip(tips.join('\n'));
}
