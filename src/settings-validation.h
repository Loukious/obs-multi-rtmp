#pragma once
#include <QScrollArea>
#include <QTabWidget>
#include <QWidget>

// Layout controls are reparented into Video/Audio tabs by both editors.
// Reveal their actual page instead of assuming every error is in Service.
inline void RevealSettingsError(QWidget* field, QWidget* message)
{
    if (!field) field = message;
    if (!field) return;
    for (auto parent = field->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto tabs = qobject_cast<QTabWidget*>(parent)) {
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->widget(i) == field || tabs->widget(i)->isAncestorOf(field)) tabs->setCurrentIndex(i);
        }
    }
    field->setFocus(Qt::OtherFocusReason);
    for (auto parent = field->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto scroll = qobject_cast<QScrollArea*>(parent)) {
            scroll->ensureWidgetVisible(field);
            if (message && scroll->widget()->isAncestorOf(message)) scroll->ensureWidgetVisible(message);
        }
    }
}
