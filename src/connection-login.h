#pragma once
#include "output-config.h"
#include <QString>
class QWidget;
// These dialogs change the supplied configuration only after success.
bool LoginTikTokWithQr(QWidget* parent, OutputTargetConfig& config);
QString DetectStreamlabsDesktopAccount(QWidget* parent);
