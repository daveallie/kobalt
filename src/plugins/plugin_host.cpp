#include "plugin_host.h"

#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QDir>
#include <QSet>
#include <algorithm>
#include <memory>
#include <vector>

namespace kobalt::plugins {
namespace {
constexpr int failureLimit = 3;

void skipped(const QString &folder, const QString &reason)
{
    diagnostic("plugin-skipped", { { "folder", folder }, { "reason", reason } },
               DiagnosticSeverity::Warning);
}
} // namespace

// Lives on the plugin thread; every member is touched only there.
class PluginHost::Worker : public QObject
{
public:
    Worker(Directory directory, Context context, Book book, const std::atomic_bool *abort,
           std::atomic_bool *opened, std::atomic_bool *answers)
        : directory(std::move(directory)),
          context(std::move(context)),
          book(std::move(book)),
          abort(abort),
          opened(opened),
          answers(answers)
    {
    }

    void open();
    Card lookup(const Lookup &lookup);
    void close();

private:
    struct Entry
    {
        std::unique_ptr<LuaPlugin> plugin;
        int failures = 0;
    };

    bool stopped() const { return abort->load(std::memory_order_relaxed); }

    void fail(Entry &entry, const QString &error);
    void publish();

    Directory directory;
    Context context;
    Book book;
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
    QSet<QString> ids;
    const auto folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &folder : folders) {
        const QString path = root.filePath(folder);
        Manifest manifest;
        QString error;
        if (!readManifest(path, &manifest, &error)) {
            skipped(path, error);
            continue;
        }
        if (ids.contains(manifest.id)) {
            skipped(path,
                    QStringLiteral("plugin ") + manifest.id
                            + QStringLiteral(" duplicates an earlier folder's id"));
            continue;
        }
        ids.insert(manifest.id);
        manifests.push_back(std::move(manifest));
    }
    std::sort(manifests.begin(), manifests.end(),
              [](const Manifest &a, const Manifest &b) { return a.id < b.id; });

    for (auto &manifest : manifests) {
        if (stopped())
            return;
        const QString path = manifest.directory;
        Entry entry;
        entry.plugin.reset(new LuaPlugin(std::move(manifest), context));
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
    plugins.clear();
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

PluginHost::PluginHost(Directory pluginDirectory, Context context, Book book, QObject *parent)
    : QObject(parent),
      worker(new Worker(std::move(pluginDirectory), std::move(context), std::move(book), &abort,
                        &opened, &answers))
{
    thread.setObjectName(QStringLiteral("plugins"));
    worker->moveToThread(&thread);
    thread.start();
    Worker *target = worker;
    qt::invoke(worker, [target] { target->open(); }, Qt::QueuedConnection);
}

PluginHost::~PluginHost()
{
    closing = true;
    abort = true;
    Worker *target = worker;
    std::atomic_bool *flag = &abort;
    // Queued lookups ahead of this call return at once; book_closed then runs unstopped.
    // Quitting after close, on the plugin thread, keeps the event loop from dropping this call.
    qt::invoke(
            worker,
            [target, flag] {
                *flag = false;
                target->close();
                QThread::currentThread()->quit();
            },
            Qt::QueuedConnection);
    {
        mainthread::BlockingOperation operation("plugins.join");
        thread.wait();
    }
    delete worker;
}

bool PluginHost::skipsLookups() const
{
    return opened.load(std::memory_order_acquire) && !answers.load(std::memory_order_relaxed);
}

void PluginHost::lookup(const Lookup &lookup, Reply reply)
{
    const quint64 request = ++requests;
    pending.insert(request, std::move(reply));
    Worker *target = worker;
    PluginHost *host = this;
    qt::invoke(
            worker,
            [target, host, request, lookup] {
                const Card card = target->lookup(lookup);
                // The destructor joins this thread before `host` is freed, and Qt drops
                // the queued reply when `host` is destroyed.
                qt::invoke(
                        host,
                        [host, request, epoch = lookup.epoch, card] {
                            const Reply reply = host->pending.take(request);
                            if (!host->closing && reply)
                                reply(epoch, card);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}
} // namespace kobalt::plugins
