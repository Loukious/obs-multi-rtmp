#include "localization.hpp"
#include <obs-module.h>

QString text(const char *key) { return QString::fromUtf8(obs_module_text(key)); }
QString translated_or(const char *key, const QString &fallback)
{
    const auto value = text(key);
    return value == QString::fromUtf8(key) ? fallback : value;
}
