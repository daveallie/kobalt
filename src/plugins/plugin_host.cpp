#include "plugin_host.h"

#include "plugin_store.h"

#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QDir>
#include <QMutex>
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
} // namespace

struct PluginHost::Shared
{
    std::atomic_bool abort{ false }, opened{ false }, answers{ false };

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
           std::shared_ptr<Shared> shared)
        : directory(std::move(directory)),
          dataDirectory(std::move(dataDirectory)),
          context(std::move(context)),
          book(std::move(book)),
          shared(std::move(shared)),
          abort(&this->shared->abort),
          opened(&this->shared->opened),
          answers(&this->shared->answers)
    {
    }

    void open();
    Card lookup(const Lookup &lookup);
    void close();
    void flush();

private:
    struct Entry
    {
        // Declared first, so it outlives the plugin that points at it.
        std::unique_ptr<PluginStore> store;
        std::unique_ptr<LuaPlugin> plugin;
        int failures = 0;
    };

    bool stopped() const { return abort->load(std::memory_order_relaxed); }

    void fail(Entry &entry, const QString &error);
    void publish();
    void save(Entry &entry);

    Directory directory, dataDirectory;
    Context context;
    Book book;
    std::shared_ptr<Shared> shared;
    const std::atomic_bool *abort;
    std::atomic_bool *opened, *answers;
    std::vector<Entry> plugins;
};

void PluginHost::Worker::open()
{
    const QString folder = directory();
    if (folder.isEmpty()) {
        publish();
        return;
    }
    QDir root(folder);
    std::vector<Manifest> manifests;
    const auto folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &folder : folders) {
        const QString path = root.filePath(folder);
        Manifest manifest;
        QString error;
        if (!readManifest(path, &manifest, &error)) {
            skipped(path, error);
            continue;
        }
        const QString mismatch = manifestFolderError(folder, manifest);
        if (!mismatch.isEmpty()) {
            skipped(path, mismatch);
            continue;
        }
        manifests.push_back(std::move(manifest));
    }
    std::sort(manifests.begin(), manifests.end(),
              [](const Manifest &a, const Manifest &b) { return a.id < b.id; });

    const QString data = dataDirectory ? dataDirectory() : QString();
    for (auto &manifest : manifests) {
        if (stopped())
            return;
        const QString path = manifest.directory;
        Entry entry;
        entry.store.reset(new PluginStore(data, manifest.id, book.key));
        entry.store->load();
        entry.plugin.reset(new LuaPlugin(std::move(manifest), context));
        entry.plugin->setStore(entry.store.get());
        entry.plugin->setAbort(abort);
        QString error;
        if (!entry.plugin->load(&error)) {
            skipped(path, error);
            continue;
        }
        plugins.push_back(std::move(entry));
    }
    for (auto &entry : plugins) {
        if (stopped())
            return;
        QString error;
        if (!entry.plugin->bookOpened(book, &error))
            fail(entry, error);
    }
    publish();
}

void PluginHost::Worker::publish()
{
    bool any = false;
    for (auto &entry : plugins)
        any = any || (entry.failures < failureLimit && entry.plugin->answersLookups());
    answers->store(any, std::memory_order_relaxed);
    opened->store(true, std::memory_order_release);
}

Card PluginHost::Worker::lookup(const Lookup &lookup)
{
    for (auto &entry : plugins) {
        if (stopped())
            return {};
        if (entry.failures >= failureLimit)
            continue;
        Card card;
        QString error;
        if (!entry.plugin->beforeLookup(lookup, book, &card, &error)) {
            fail(entry, error);
            continue;
        }
        if (!card.isEmpty())
            return card;
    }
    return {};
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

void PluginHost::Worker::fail(Entry &entry, const QString &error)
{
    ++entry.failures;
    diagnostic("plugin-failed", { { "message", error }, { "failures", entry.failures } },
               DiagnosticSeverity::Warning);
    if (entry.failures == failureLimit) {
        diagnostic("plugin-disabled", { { "plugin", entry.plugin->manifest().id } },
                   DiagnosticSeverity::Warning);
        if (opened->load(std::memory_order_relaxed))
            publish();
    }
}

PluginHost::PluginHost(Directory pluginDirectory, Directory dataDirectory, Context context,
                       Book book, int joinLimitMs, QObject *parent)
    : QObject(parent), shared(std::make_shared<Shared>()), joinLimitMs(joinLimitMs)
{
    shared->host = this;
    if (detached.load() > 0) {
        diagnostic(
                "plugins-off",
                { { "reason", QStringLiteral("a plugin thread from an earlier book still runs") } },
                DiagnosticSeverity::Warning);
        shared->opened.store(true, std::memory_order_release);
        return;
    }
    worker = new Worker(std::move(pluginDirectory), std::move(dataDirectory), std::move(context),
                        std::move(book), shared);
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

int PluginHost::detachedThreads()
{
    return detached.load();
}

bool PluginHost::skipsLookups() const
{
    return shared->opened.load(std::memory_order_acquire)
            && !shared->answers.load(std::memory_order_relaxed);
}

void PluginHost::lookup(const Lookup &lookup, Reply reply)
{
    const quint64 request = ++requests;
    pending.insert(request, std::move(reply));
    PluginHost *self = this;
    const auto answer = [self, request, epoch = lookup.epoch](const Card &card) {
        return [self, request, epoch, card] {
            const Reply reply = self->pending.take(request);
            if (!self->closing && reply)
                reply(epoch, card);
        };
    };
    if (!worker) {
        qt::invoke(this, answer(Card()), Qt::QueuedConnection);
        return;
    }
    Worker *target = worker;
    std::shared_ptr<Shared> state = shared;
    qt::invoke(
            worker,
            [target, state, lookup, answer] {
                const Card card = target->lookup(lookup);
                // Qt drops a queued reply when the host is destroyed; the lock keeps the
                // host from being destroyed while the reply is posted.
                QMutexLocker lock(&state->mutex);
                if (state->host)
                    qt::invoke(state->host, answer(card), Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}
} // namespace kobalt::plugins
