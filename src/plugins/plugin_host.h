#pragma once
#include "lua_plugin.h"
#include "plugin_types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>
#include <functional>

namespace kobalt::plugins {
// Runs every plugin on one dedicated thread. Resolving and reading the plugin
// folder, loading and book_opened run there after construction, and each
// lookup waits in line behind them. The main thread never reads the plugin
// folder and never waits for the plugin thread, except in the destructor.
class PluginHost : public QObject
{
public:
    // An empty card means that no plugin answered.
    using Reply = std::function<void(quint64 epoch, Card card)>;
    // Runs on the plugin thread; an empty folder means no plugins.
    using Directory = std::function<QString()>;

    PluginHost(Directory pluginDirectory, Context context, Book book, QObject *parent = nullptr);
    // Stops a running hook, runs book_closed and joins the thread. Call it
    // only where blocking the main thread is allowed.
    ~PluginHost() override;
    PluginHost(const PluginHost &) = delete;
    PluginHost &operator=(const PluginHost &) = delete;

    // Main thread. `reply` runs on the main thread exactly once, unless the
    // host is destroyed first.
    void lookup(const Lookup &lookup, Reply reply);
    // True once the plugins have opened and none can answer a lookup, so a
    // lookup need not wait for the plugin thread. Any thread.
    bool skipsLookups() const;

private:
    class Worker;

    QThread thread;
    Worker *worker;
    std::atomic_bool abort{ false };
    std::atomic_bool opened{ false }, answers{ false };
    bool closing = false;
    quint64 requests = 0;
    // Replies stay on the main thread; the plugin thread sees only request numbers.
    QHash<quint64, Reply> pending;
};
} // namespace kobalt::plugins
