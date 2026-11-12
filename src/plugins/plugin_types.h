#pragma once
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace kobalt::plugins {
struct Book
{
    QString identity, title, language;
    // BookmarkStore::bookKey, which keeps a sideloaded book's data when its file moves.
    QString key;
};

struct Position
{
    double percent = 0;
    int page = 0, pages = 0;
    QString chapter;
};

struct Card
{
    // The answering plugin, filled in by the host.
    QString pluginId, pluginName;
    QString title, html, footer;

    bool isEmpty() const { return title.isEmpty(); }
};

// One row of the plugin list: an installed folder, or saved data without one.
struct PluginStatus
{
    enum State { Running, TurnedOff, Stopped, NotLoaded, DataOnly };

    QString id, folder, name, version, description;
    // Why a NotLoaded plugin did not load.
    QString reason;
    State state = NotLoaded;
};

// The user's choices: `enabled` false loads no plugin, and `disabled` names plugins to skip.
struct Switches
{
    bool enabled = true;
    QSet<QString> disabled;
};

struct Lookup
{
    quint64 epoch = 0;
    QString word;
    Position position;
};
} // namespace kobalt::plugins
