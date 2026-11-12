#pragma once
#include "plugin_types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>

namespace kobalt::plugins {
// Reads `plugins_enabled` and `plugins_disabled` from global preferences.
inline Switches switchesFrom(const QJsonObject &preferences)
{
    Switches switches;
    switches.enabled = preferences.value(QStringLiteral("plugins_enabled")).toBool(true);
    for (const auto &id : preferences.value(QStringLiteral("plugins_disabled")).toArray())
        if (id.isString())
            switches.disabled.insert(id.toString());
    return switches;
}

inline void storeSwitches(QJsonObject &preferences, const Switches &switches)
{
    preferences[QStringLiteral("plugins_enabled")] = switches.enabled;
    QStringList ids = switches.disabled.values();
    std::sort(ids.begin(), ids.end());
    preferences[QStringLiteral("plugins_disabled")] = QJsonArray::fromStringList(ids);
}
} // namespace kobalt::plugins
