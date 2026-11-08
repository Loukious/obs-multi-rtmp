#pragma once
#include "hosted_signing_service.hpp"
#include "tiktok_studio_account.hpp"
#include <QHash>

// Passive telemetry only: neither recording nor reading quota sends requests.
RapidApiQuota ParseRapidApiQuota(const QHash<QByteArray, QByteArray>& headers, qint64 observed);
RapidApiQuota RecordRapidApiQuota(const HostedSigningServiceConfig& service,
    const QHash<QByteArray, QByteArray>& headers);
RapidApiQuota ReadRapidApiQuota(const HostedSigningServiceConfig& service);
// Empty directory keeps snapshots in memory (used by standalone tests).
void SetRapidApiQuotaDirectory(const QString& directory);
