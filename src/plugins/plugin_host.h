#pragma once
#include "lua_plugin.h"
#include "plugin_types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThread>
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
    // An empty card means that no plugin answered.
    using Reply = std::function<void(quint64 epoch, Card card)>;
    // Runs on the plugin thread; an empty folder means no plugins.
    using Directory = std::function<QString()>;

    // `dataDirectory` holds every plugin's saved data; empty means none is saved.
    PluginHost(Directory pluginDirectory, Directory dataDirectory, Context context, Book book,
               int joinLimitMs = 2000, QObject *parent = nullptr);
    // Stops a running hook, runs book_closed and waits up to the join limit
    // for the thread; a thread still running then is left to finish on its
    // own. Call it only where blocking the main thread is allowed.
    ~PluginHost() override;
    PluginHost(const PluginHost &) = delete;
    PluginHost &operator=(const PluginHost &) = delete;

    // Main thread. `reply` runs on the main thread exactly once, unless the
    // host is destroyed first.
    void lookup(const Lookup &lookup, Reply reply);
    // True once the plugins have opened and none can answer a lookup, so a
    // lookup need not wait for the plugin thread. Any thread.
    bool skipsLookups() const;
    // Main thread. Queues a save of the plugin data that changed and never waits.
    void flush();
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
    // Replies stay on the main thread; the plugin thread sees only request numbers.
    QHash<quint64, Reply> pending;
};
} // namespace kobalt::plugins
