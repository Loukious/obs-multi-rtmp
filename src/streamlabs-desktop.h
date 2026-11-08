#pragma once
#include <QString>
#include <atomic>
// Same newest-log / last-token selection as the original generator.
QString FindStreamlabsDesktopToken(const QString& directory, const std::atomic_bool& cancelled);
QString StreamlabsDesktopDirectory();
