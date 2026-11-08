#pragma once
#include "output-config.h"
#include <QString>
#include <atomic>
#include <functional>

struct AccountInfoResult {
    QString username;
    const char *errorKey = nullptr;
    QString applicationStatus;
    std::optional<bool> canGoLive;
    QString dualLayoutStatus;
    std::optional<bool> canDualLayout;
    nlohmann::json deviceProfile = nullptr;
    QString errorMessage;
};
using AccountInfoLookup = std::function<AccountInfoResult(const OutputTargetConfig &, const std::atomic_bool &)>;

// Registers a missing Android identity before lookup and returns its profile
// for the editor to save. Never creates rooms or persists cookies.
AccountInfoResult LookupAccountInfo(const OutputTargetConfig &config, const std::atomic_bool &cancelled);
