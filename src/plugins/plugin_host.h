#pragma once
#include "lua_plugin.h"
#include "plugin_types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>
#include <functional>
#include <memory>

namespace kobalt::plugins {
// Runs every plugin on one dedicated thread. Resolving and reading the plugin
// folder, loading and book_opened run there after construction, and each
// lookup waits in line behind them. The main thread never reads the plugin
// folder and never waits for the plugin thread, except for a bounded wait in
// the destructor. While a thread left behind by that wait still runs, new
// hosts start with plugins off.
class PluginHost : public QObject
{
public:
    // Runs once for each plugin that answers, as soon as it answers.
    using Reply = std::function<void(quint64 epoch, Card card)>;
    // Runs once, after the last plugin.
    using Done = std::function<void(quint64 epoch)>;
    // Runs on the plugin thread; an empty folder means no plugins.
    using Directory = std::function<QString()>;

    // `dataDirectory` holds every plugin's saved data; empty means none is saved.
    // `switches` decides which plugins load; every folder's manifest is still read
    // for the status list.
    PluginHost(Directory pluginDirectory, Directory dataDirectory, Context context, Book book,
               Switches switches = {}, int joinLimitMs = 2000, QObject *parent = nullptr);
    // Stops a running hook, runs book_closed and waits up to the join limit
    // for the thread; a thread still running then is left to finish on its
    // own. Call it only where blocking the main thread is allowed.
    ~PluginHost() override;
    PluginHost(const PluginHost &) = delete;
    PluginHost &operator=(const PluginHost &) = delete;

    // Main thread. `each` and `done` run on the main thread, unless the host is
    // destroyed first. A lookup whose epoch is older than a later lookup's when
    // the plugin thread reaches it runs no plugin.
    void lookup(const Lookup &lookup, Reply each, Done done);
    // True once the plugins have opened and none can answer a lookup, so a
    // lookup need not wait for the plugin thread. Any thread.
    bool skipsLookups() const;
    // Main thread. Queues a save of the plugin data that changed and never waits.
    void flush();
    // Main thread. Removes one plugin's saved data, including a running plugin's,
    // and reports on the main thread.
    void deleteData(const QString &id, std::function<void(bool ok, QString error)> done);
    // The latest status list, empty until the plugin thread has read the folder.
    QVector<PluginStatus> statuses() const;
    // Main thread; called whenever the status list changes.
    std::function<void(const QVector<PluginStatus> &)> statusChanged;
    // True when plugins are off because a thread from an earlier book still runs.
    bool pluginsBlocked() const;
    // Plugin threads that outlived their host and still run. Any thread.
    static int detachedThreads();

private:
    class Worker;
    struct Shared;

    // Both null while plugins are off. Neither has a parent: a detached pair
    // deletes itself when the thread finishes.
    QThread *thread = nullptr;
    Worker *worker = nullptr;
    // Everything that the plugin thread may touch after this host is gone.
    std::shared_ptr<Shared> shared;
    int joinLimitMs;
    bool closing = false;
    quint64 requests = 0;

    struct Pending
    {
        Reply each;
        Done done;
    };

    void post(const std::function<void()> &call);

    // Replies stay on the main thread; the plugin thread sees only request numbers.
    QHash<quint64, Pending> pending;
    QVector<PluginStatus> snapshot;
    bool blocked = false;
};
} // namespace kobalt::plugins
