#include "plugin_host.h"

#include "plugin_store.h"

#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QDir>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace kobalt::plugins {
namespace {
constexpr int failureLimit = 3;
std::atomic_int detached{ 0 };

void skipped(const QString &folder, const QString &reason)
{
    diagnostic("plugin-skipped", { { "folder", folder }, { "reason", reason } },
               DiagnosticSeverity::Warning);
}

// A folder name that cannot leave the plugin data folder.
bool plainId(const QString &id)
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9._-]+\\z"));
    return valid.match(id).hasMatch() && id != QLatin1String(".") && id != QLatin1String("..");
}
} // namespace

struct PluginHost::Shared
{
    std::atomic_bool abort{ false }, opened{ false }, answers{ false };
    // The newest lookup epoch the main thread has sent.
    std::atomic<quint64> latest{ 0 };

    enum State { Attached, Detached, Finished };

    std::atomic_int state{ Attached };
    // Guards `host`, which is null once the host is gone, while a reply is posted to it.
    QMutex mutex;
    PluginHost *host = nullptr;
};

// Lives on the plugin thread; every member is touched only there.
class PluginHost::Worker : public QObject
{
public:
    Worker(Directory directory, Directory dataDirectory, Context context, Book book,
           Switches switches, std::shared_ptr<Shared> shared)
        : directory(std::move(directory)),
          dataDirectory(std::move(dataDirectory)),
          context(std::move(context)),
          book(std::move(book)),
          switches(std::move(switches)),
          shared(std::move(shared)),
          abort(&this->shared->abort),
          opened(&this->shared->opened),
          answers(&this->shared->answers)
    {
    }

    void open();
    // Calls `answer` with each card as soon as its plugin returns it.
    void lookup(const Lookup &lookup, const std::function<void(const Card &)> &answer);
    void close();
    void flush();
    bool deleteData(const QString &id, QString *error);

private:
    struct Entry
    {
        // Declared first, so it outlives the plugin that points at it.
        std::unique_ptr<PluginStore> store;
        std::unique_ptr<LuaPlugin> plugin;
        int failures = 0;
        // Index of this plugin's row in `statuses`.
        int status = 0;
    };

    bool stopped() const { return abort->load(std::memory_order_relaxed); }

    void fail(Entry &entry, const QString &error);
    void publish();
    void publishStatuses();
    void addDataOnly();
    void save(Entry &entry);

    Directory directory, dataDirectory;
    Context context;
    Book book;
    Switches switches;
    std::shared_ptr<Shared> shared;
    const std::atomic_bool *abort;
    std::atomic_bool *opened, *answers;
    std::vector<Entry> plugins;
    QVector<PluginStatus> statuses;
    QString dataRoot;
};

void PluginHost::Worker::open()
{
    const QString folder = directory();
    dataRoot = dataDirectory ? dataDirectory() : QString();
    if (folder.isEmpty()) {
        addDataOnly();
        publishStatuses();
        publish();
        return;
    }
    QDir root(folder);
    std::vector<Manifest> manifests;
    const auto folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : folders) {
        const QString path = root.filePath(name);
        PluginStatus status;
        status.id = name;
        status.name = name;
        status.folder = QDir::cleanPath(path);
        Manifest manifest;
        QString error;
        if (!readManifest(path, &manifest, &error)) {
            skipped(path, error);
            status.reason = error;
            statuses.append(status);
            continue;
        }
        const QString mismatch = manifestFolderError(name, manifest);
        if (!mismatch.isEmpty()) {
            skipped(path, mismatch);
            status.reason = mismatch;
            statuses.append(status);
            continue;
        }
        status.name = manifest.name;
        status.version = manifest.version;
        status.description = manifest.description;
        if (!switches.enabled || switches.disabled.contains(manifest.id)) {
            status.state = PluginStatus::TurnedOff;
            statuses.append(status);
            continue;
        }
        statuses.append(status);
        manifests.push_back(std::move(manifest));
    }
    std::sort(manifests.begin(), manifests.end(),
              [](const Manifest &a, const Manifest &b) { return a.id < b.id; });

    for (auto &manifest : manifests) {
        if (stopped())
            return;
        const QString path = manifest.directory;
        int row = 0;
        while (statuses[row].id != manifest.id)
            ++row;
        Entry entry;
        entry.status = row;
        entry.store.reset(new PluginStore(dataRoot, manifest.id, book.key));
        entry.store->load();
        entry.plugin.reset(new LuaPlugin(std::move(manifest), context));
        entry.plugin->setStore(entry.store.get());
        entry.plugin->setAbort(abort);
        QString error;
        if (!entry.plugin->load(&error)) {
            skipped(path, error);
            statuses[row].reason = error;
            continue;
        }
        statuses[row].state = PluginStatus::Running;
        plugins.push_back(std::move(entry));
    }
    addDataOnly();
    publishStatuses();
    for (auto &entry : plugins) {
        if (stopped())
            return;
        QString error;
        if (!entry.plugin->bookOpened(book, &error))
            fail(entry, error);
    }
    publish();
}

// Saved data whose plugin folder is gone still gets a row, so it can be deleted.
void PluginHost::Worker::addDataOnly()
{
    if (dataRoot.isEmpty())
        return;
    QSet<QString> installed;
    for (const PluginStatus &status : statuses)
        installed.insert(QDir(status.folder).dirName());
    const auto folders = QDir(dataRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : folders) {
        if (installed.contains(name))
            continue;
        PluginStatus status;
        status.id = name;
        status.name = name;
        status.state = PluginStatus::DataOnly;
        statuses.append(status);
    }
}

void PluginHost::Worker::publishStatuses()
{
    const QVector<PluginStatus> snapshot = statuses;
    QMutexLocker lock(&shared->mutex);
    PluginHost *host = shared->host;
    if (!host)
        return;
    qt::invoke(
            host,
            [host, snapshot] {
                host->snapshot = snapshot;
                if (host->statusChanged)
                    host->statusChanged(snapshot);
            },
            Qt::QueuedConnection);
}

void PluginHost::Worker::publish()
{
    bool any = false;
    for (auto &entry : plugins)
        any = any || (entry.failures < failureLimit && entry.plugin->answersLookups());
    answers->store(any, std::memory_order_relaxed);
    opened->store(true, std::memory_order_release);
}

void PluginHost::Worker::lookup(const Lookup &lookup,
                                const std::function<void(const Card &)> &answer)
{
    if (lookup.epoch < shared->latest.load(std::memory_order_relaxed))
        return;
    for (auto &entry : plugins) {
        if (stopped())
            return;
        if (entry.failures >= failureLimit)
            continue;
        Card card;
        QString error;
        if (!entry.plugin->beforeLookup(lookup, book, &card, &error)) {
            fail(entry, error);
            continue;
        }
        if (card.isEmpty())
            continue;
        card.pluginId = entry.plugin->manifest().id;
        card.pluginName = entry.plugin->manifest().name;
        answer(card);
    }
}

void PluginHost::Worker::close()
{
    for (auto &entry : plugins) {
        if (entry.failures >= failureLimit)
            continue;
        QString error;
        if (!entry.plugin->bookClosed(&error))
            fail(entry, error);
    }
    for (auto &entry : plugins)
        save(entry);
    plugins.clear();
}

void PluginHost::Worker::flush()
{
    for (auto &entry : plugins)
        save(entry);
}

void PluginHost::Worker::save(Entry &entry)
{
    QString error;
    if (!entry.store->save(&error))
        diagnostic("plugin-data-save-failed",
                   { { "plugin", entry.plugin->manifest().id }, { "message", error } },
                   DiagnosticSeverity::Warning);
}

bool PluginHost::Worker::deleteData(const QString &id, QString *error)
{
    if (!plainId(id)) {
        *error = QStringLiteral("not a plugin id: ") + id;
        return false;
    }
    if (dataRoot.isEmpty()) {
        *error = QStringLiteral("no plugin data folder");
        return false;
    }
    bool ok = false;
    bool running = false;
    for (auto &entry : plugins) {
        if (entry.plugin->manifest().id == id) {
            running = true;
            ok = entry.store->clear(error);
        }
    }
    if (!running)
        ok = PluginStore(dataRoot, id, QString()).clear(error);
    for (int row = statuses.size() - 1; row >= 0; --row) {
        if (statuses[row].state == PluginStatus::DataOnly && statuses[row].id == id
            && !QDir(dataRoot + QLatin1Char('/') + id).exists())
            statuses.remove(row);
    }
    publishStatuses();
    return ok;
}

void PluginHost::Worker::fail(Entry &entry, const QString &error)
{
    ++entry.failures;
    diagnostic("plugin-failed", { { "message", error }, { "failures", entry.failures } },
               DiagnosticSeverity::Warning);
    if (entry.failures == failureLimit) {
        diagnostic("plugin-disabled", { { "plugin", entry.plugin->manifest().id } },
                   DiagnosticSeverity::Warning);
        statuses[entry.status].state = PluginStatus::Stopped;
        publishStatuses();
        if (opened->load(std::memory_order_relaxed))
            publish();
    }
}

PluginHost::PluginHost(Directory pluginDirectory, Directory dataDirectory, Context context,
                       Book book, Switches switches, int joinLimitMs, QObject *parent)
    : QObject(parent), shared(std::make_shared<Shared>()), joinLimitMs(joinLimitMs)
{
    shared->host = this;
    if (detached.load() > 0) {
        diagnostic(
                "plugins-off",
                { { "reason", QStringLiteral("a plugin thread from an earlier book still runs") } },
                DiagnosticSeverity::Warning);
        blocked = true;
        shared->opened.store(true, std::memory_order_release);
        return;
    }
    worker = new Worker(std::move(pluginDirectory), std::move(dataDirectory), std::move(context),
                        std::move(book), std::move(switches), shared);
    thread = new QThread;
    thread->setObjectName(QStringLiteral("plugins"));
    worker->moveToThread(thread);
    Worker *target = worker;
    std::shared_ptr<Shared> state = shared;
    // Without a context this runs directly on the plugin thread after its event
    // loop ends, so the worker is idle.
    QObject::connect(thread, &QThread::finished, [target, state] {
        delete target;
        if (state->state.exchange(Shared::Finished) == Shared::Detached)
            --detached;
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    qt::invoke(worker, [target] { target->open(); }, Qt::QueuedConnection);
}

PluginHost::~PluginHost()
{
    closing = true;
    if (!thread)
        return;
    shared->abort = true;
    Worker *target = worker;
    std::shared_ptr<Shared> state = shared;
    // Queued lookups ahead of this call return at once; book_closed then runs unstopped,
    // unless the host has given up on the thread meanwhile.
    // Quitting after close, on the plugin thread, keeps the event loop from dropping this call.
    qt::invoke(
            worker,
            [target, state] {
                state->abort = false;
                if (state->state.load() != Shared::Attached)
                    state->abort = true;
                target->close();
                QThread::currentThread()->quit();
            },
            Qt::QueuedConnection);
    bool joined = false;
    {
        mainthread::BlockingOperation operation("plugins.join");
        joined = thread->wait(static_cast<unsigned long>(qMax(0, joinLimitMs)));
        QMutexLocker lock(&shared->mutex);
        shared->host = nullptr;
    }
    if (joined) {
        delete thread;
        return;
    }
    ++detached;
    if (shared->state.exchange(Shared::Detached) == Shared::Finished) {
        --detached;
        return;
    }
    shared->abort = true;
    diagnostic("plugin-hang", { { "waited_ms", joinLimitMs } }, DiagnosticSeverity::Warning);
}

void PluginHost::flush()
{
    if (!worker || closing)
        return;
    Worker *target = worker;
    qt::invoke(worker, [target] { target->flush(); }, Qt::QueuedConnection);
}

QVector<PluginStatus> PluginHost::statuses() const
{
    return snapshot;
}

bool PluginHost::pluginsBlocked() const
{
    return blocked;
}

int PluginHost::detachedThreads()
{
    return detached.load();
}

bool PluginHost::skipsLookups() const
{
    return shared->opened.load(std::memory_order_acquire)
            && !shared->answers.load(std::memory_order_relaxed);
}

// Runs `call` on the main thread unless the host is gone; the plugin thread calls it.
void PluginHost::post(const std::function<void()> &call)
{
    qt::invoke(this, call, Qt::QueuedConnection);
}

void PluginHost::deleteData(const QString &id, std::function<void(bool ok, QString error)> done)
{
    if (!worker || closing) {
        qt::invoke(
                this, [done] { done(false, QStringLiteral("plugins are off")); },
                Qt::QueuedConnection);
        return;
    }
    Worker *target = worker;
    std::shared_ptr<Shared> state = shared;
    qt::invoke(
            worker,
            [target, state, id, done] {
                QString error;
                const bool ok = target->deleteData(id, &error);
                QMutexLocker lock(&state->mutex);
                if (state->host)
                    state->host->post([done, ok, error] { done(ok, error); });
            },
            Qt::QueuedConnection);
}

void PluginHost::lookup(const Lookup &lookup, Reply each, Done done)
{
    const quint64 request = ++requests;
    pending.insert(request, { std::move(each), std::move(done) });
    if (lookup.epoch > shared->latest.load(std::memory_order_relaxed))
        shared->latest.store(lookup.epoch, std::memory_order_relaxed);
    PluginHost *self = this;
    const quint64 epoch = lookup.epoch;
    const auto finish = [self, request, epoch] {
        const Pending reply = self->pending.take(request);
        if (!self->closing && reply.done)
            reply.done(epoch);
    };
    if (!worker) {
        qt::invoke(this, finish, Qt::QueuedConnection);
        return;
    }
    Worker *target = worker;
    std::shared_ptr<Shared> state = shared;
    qt::invoke(
            worker,
            [target, state, self, request, lookup, finish] {
                // Qt drops a queued reply when the host is destroyed; the lock keeps the
                // host from being destroyed while a reply is posted.
                target->lookup(
                        lookup, [state, self, request, epoch = lookup.epoch](const Card &card) {
                            QMutexLocker lock(&state->mutex);
                            if (!state->host)
                                return;
                            state->host->post([self, request, epoch, card] {
                                const auto it = self->pending.constFind(request);
                                if (!self->closing && it != self->pending.constEnd() && it->each)
                                    it->each(epoch, card);
                            });
                        });
                QMutexLocker lock(&state->mutex);
                if (state->host)
                    state->host->post(finish);
            },
            Qt::QueuedConnection);
}
} // namespace kobalt::plugins
