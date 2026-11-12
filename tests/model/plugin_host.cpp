#include "plugins/plugin_host.h"

#include "check.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <memory>

using namespace kobalt;
using namespace kobalt::plugins;

namespace {
// Marks event delivery so that the guard checks what the host does inside events.
class Application : public QCoreApplication
{
public:
    using QCoreApplication::QCoreApplication;

    bool notify(QObject *object, QEvent *event) override
    {
        mainthread::EventDelivery delivery;
        return QCoreApplication::notify(object, event);
    }
};

QString fixture(const QString &name)
{
    return QStringLiteral(KOBALT_TEST_PLUGIN_FIXTURES "/") + name;
}

template <class Condition>
bool waitFor(Condition done)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done()) {
        if (elapsed.elapsed() > 10000)
            return false;
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return true;
}

// Runs `work` inside a delivered event, as the reader's own calls run.
template <class Work>
void inEvent(Work work)
{
    bool done = false;
    qt::invoke(
            qApp,
            [&] {
                work();
                done = true;
            },
            Qt::QueuedConnection);
    require(waitFor([&] { return done; }), "A queued test step never ran");
}

std::unique_ptr<PluginHost> open(PluginHost::Directory directory, int joinLimitMs = 2000,
                                 QString dataRoot = {}, QString bookKey = QStringLiteral("key"))
{
    std::unique_ptr<PluginHost> host;
    inEvent([&] {
        host.reset(new PluginHost(
                std::move(directory), [dataRoot] { return dataRoot; }, Context(),
                { QStringLiteral("id"), QStringLiteral("Wolf Hall"), QStringLiteral("en"),
                  bookKey },
                joinLimitMs));
    });
    return host;
}

std::unique_ptr<PluginHost> open(const QString &directory, int joinLimitMs = 2000)
{
    return open([directory] { return directory; }, joinLimitMs);
}

std::unique_ptr<PluginHost> openWithData(const QString &directory, const QString &dataRoot,
                                         const QString &bookKey = QStringLiteral("key"))
{
    return open([directory] { return directory; }, 2000, dataRoot, bookKey);
}

void close(std::unique_ptr<PluginHost> &host)
{
    inEvent([&] {
        mainthread::AllowBlocking allow("closing reader");
        host.reset();
    });
}

struct Answer
{
    bool replied = false;
    quint64 epoch = 0;
    Card card;
    bool mainThread = false;
};

std::shared_ptr<Answer> ask(PluginHost &host, const QString &word, quint64 epoch = 1)
{
    auto answer = std::make_shared<Answer>();
    inEvent([&] {
        Lookup lookup;
        lookup.epoch = epoch;
        lookup.word = word;
        host.lookup(lookup, [answer](quint64 replyEpoch, Card card) {
            require(!answer->replied, "A lookup replied twice");
            answer->replied = true;
            answer->epoch = replyEpoch;
            answer->card = card;
            answer->mainThread = QThread::currentThread() == qApp->thread();
        });
    });
    return answer;
}

Card answered(PluginHost &host, const QString &word = QStringLiteral("Cromwell"))
{
    const auto answer = ask(host, word);
    require(waitFor([&] { return answer->replied; }), "A lookup never replied");
    return answer->card;
}

// Collects plugin diagnostics, which arrive on the plugin thread.
struct Events
{
    QMutex mutex;
    QStringList lines;
    std::atomic_bool looping{ false }, closed{ false }, sorting{ false };

    QStringList take()
    {
        QMutexLocker lock(&mutex);
        const QStringList result = lines;
        lines.clear();
        return result;
    }
};

void discoveryOrder(Events &events)
{
    auto host = open(fixture("order"));
    const Card card = answered(*host);
    require(card.title == "a", "The plugin with the first id did not answer first");
    close(host);
    events.take();
}

void folderMustMatchId(Events &events)
{
    events.take();
    auto host = open(fixture("mismatch"));
    require(answered(*host).isEmpty(), "A plugin whose folder differs from its id answered");
    close(host);
    require(events.take().filter("plugin-skipped")
                            .filter("folder name must match id 'right'")
                            .size()
                    == 1,
            "A folder that differs from its id was not logged");
}

void silentFallsThrough()
{
    auto host = open(fixture("silent"));
    require(answered(*host).title == "b", "A plugin that returned nil hid the next plugin");
    close(host);
}

void noPluginsRepliesNothing()
{
    QTemporaryDir empty;
    for (const QString &directory : { empty.path(), empty.path() + "/missing" }) {
        auto host = open(directory);
        QElapsedTimer elapsed;
        elapsed.start();
        const auto answer = ask(*host, QStringLiteral("Cromwell"), 9);
        require(waitFor([&] { return answer->replied; }), "A host without plugins never replied");
        require(elapsed.elapsed() < 1000, "A host without plugins replied late");
        require(answer->card.isEmpty() && answer->epoch == 9,
                "A host without plugins returned a card or lost the epoch");
        close(host);
    }
}

void folderResolvedOffMainThread()
{
    auto resolved = std::make_shared<std::atomic_int>(0);
    auto onMain = std::make_shared<std::atomic_bool>(true);
    QThread *main = QThread::currentThread();
    auto host = open([resolved, onMain, main] {
        onMain->store(QThread::currentThread() == main);
        ++*resolved;
        return QString(KOBALT_TEST_PLUGIN_FIXTURES "/single");
    });
    require(answered(*host).title == "Cromwell", "A resolved plugin folder was not used");
    require(resolved->load() == 1 && !onMain->load(),
            "The plugin folder was not resolved once on the plugin thread");
    close(host);

    auto none = open([] { return QString(); });
    require(waitFor([&] { return none->skipsLookups(); }),
            "An empty resolved folder did not leave lookups to the dictionary");
    close(none);
}

void skipsWithoutBeforeLookup()
{
    auto quiet = open(fixture("hookless"));
    require(waitFor([&] { return quiet->skipsLookups(); }),
            "Plugins without before_lookup still took lookups");
    close(quiet);

    auto single = open(fixture("single"));
    require(answered(*single).title == "Cromwell", "A plugin with before_lookup did not answer");
    require(!single->skipsLookups(), "A plugin with before_lookup was skipped");
    close(single);
}

void invalidManifestsSkipped(Events &events)
{
    events.take();
    auto host = open(fixture("invalid"));
    require(answered(*host).title == "good", "A good plugin beside invalid ones did not answer");
    close(host);
    const QStringList skipped = events.take().filter("plugin-skipped");
    for (const char *folder : { "bad_manifest", "no_main", "load_error" })
        require(skipped.filter(QStringLiteral("invalid/") + folder).size() == 1,
                "A skipped plugin folder was not logged once");
    require(skipped.filter("invalid/good").isEmpty(), "A good plugin was logged as skipped");
}

void threeFailuresDisable(Events &events)
{
    events.take();
    auto host = open(fixture("failures"));
    for (int call = 1; call <= 3; ++call)
        require(answered(*host).isEmpty(), "A failing plugin returned a card");
    require(answered(*host).isEmpty(), "A plugin that failed three times still ran");
    require(host->skipsLookups(), "Lookups still waited for a disabled plugin");
    close(host);
    const QStringList lines = events.take();
    require(lines.filter("plugin-failed").size() == 3, "Each plugin failure was not logged");
    require(lines.filter("plugin-disabled").filter("flaky").size() == 1,
            "The disabled plugin was not logged");
}

void queuedBehindSlowOpen()
{
    auto host = open(fixture("slow_open"));
    require(answered(*host).title == "after", "A lookup ran before book_opened finished");
    close(host);
}

void destroyWhileRunning(Events &events)
{
    events.looping = false;
    auto host = open(fixture("endless"));
    const auto answer = ask(*host, QStringLiteral("Cromwell"));
    require(waitFor([&] { return events.looping.load(); }), "The endless hook never started");
    QElapsedTimer elapsed;
    elapsed.start();
    close(host);
    require(elapsed.elapsed() < 2000, "Destroying the host did not stop a running hook");
    QCoreApplication::processEvents();
    require(!answer->replied, "A lookup replied after its host was destroyed");
}

void closeRunsBookClosed(Events &events)
{
    events.closed = false;
    auto host = open(fixture("closing"));
    require(waitFor([&] { return host->skipsLookups(); }), "The closing fixture never opened");
    close(host);
    require(events.closed, "Destroying the host did not run book_closed");
    require(PluginHost::detachedThreads() == 0, "A host that joined left a thread behind");
}

void hungCloseDetaches(Events &events)
{
    events.take();
    events.sorting = false;
    auto host = open(fixture("hang"), 1);
    require(waitFor([&] { return host->skipsLookups(); }), "The hang fixture never opened");
    QElapsedTimer elapsed;
    elapsed.start();
    close(host);
    require(elapsed.elapsed() < 1000, "Destroying a host with a hung book_closed did not return");
    require(PluginHost::detachedThreads() == 1, "The hung plugin thread was not left running");
    require(events.take().filter("plugin-hang").size() == 1, "The hang was not logged");

    auto next = open(fixture("single"));
    require(next->skipsLookups(),
            "A host opened beside a hung thread did not start with plugins off");
    require(answered(*next).isEmpty(), "A host with plugins off returned a card");
    close(next);
    require(events.take().filter("plugins-off").size() == 1, "Plugins off was not logged");

    require(waitFor([] { return PluginHost::detachedThreads() == 0; }),
            "The detached plugin thread never finished");
    require(events.sorting, "The detached thread did not run book_closed");
    auto after = open(fixture("single"));
    require(answered(*after).title == "Cromwell", "Plugins stayed off after the hung thread ended");
    close(after);
}

void storePersistsAcrossBooks()
{
    QTemporaryDir data;
    auto host = openWithData(fixture("remember"), data.path());
    require(answered(*host).title == "book 1 global 1", "The first lookup was not counted");
    require(answered(*host).title == "book 2 global 2", "The second lookup was not counted");
    close(host);
    host = openWithData(fixture("remember"), data.path());
    require(answered(*host).title == "book 3 global 3", "Plugin data did not survive closing");
    close(host);
    host = openWithData(fixture("remember"), data.path(), QStringLiteral("other"));
    require(answered(*host).title == "book 1 global 4",
            "Another book shared this book's data or lost the global data");
    close(host);
}

void sleepFlushes()
{
    QTemporaryDir data;
    const QString global = data.path() + "/remember/global.json";
    auto host = openWithData(fixture("remember"), data.path());
    require(answered(*host).title == "book 1 global 1", "The lookup was not counted");
    inEvent([&] { host->flush(); });
    require(waitFor([&] { return QFile::exists(global); }), "Sleep did not write the store");
    const QDateTime old = QDateTime::fromSecsSinceEpoch(1'000'000'000);
    {
        QFile file(global);
        require(file.open(QIODevice::ReadWrite)
                        && file.setFileTime(old, QFileDevice::FileModificationTime),
                "The modification time was not set");
    }
    inEvent([&] { host->flush(); });
    close(host);
    require(QFileInfo(global).lastModified() == old,
            "A sleep or close without changes rewrote the store");
}

void noDataRootStillAnswers(Events &events)
{
    events.take();
    auto host = openWithData(fixture("remember"), QString());
    require(answered(*host).title == "book 1 global 1", "A plugin without data did not answer");
    require(answered(*host).title == "book 1 global 1", "A plugin without data kept values");
    close(host);
    require(events.take().filter("plugin-failed").isEmpty(),
            "A refused store write counted as a plugin failure");
}

void replyOnMainThread()
{
    auto host = open(fixture("single"));
    const auto answer = ask(*host, QStringLiteral("Cromwell"), 42);
    require(waitFor([&] { return answer->replied; }), "A lookup never replied");
    require(answer->mainThread, "A reply ran off the main thread");
    require(answer->epoch == 42 && answer->card.title == "Cromwell",
            "A reply lost its epoch or card");
    close(host);
}
} // namespace

int main(int argc, char **argv)
{
    qputenv("KOBALT_STRICT_MAIN_THREAD", "1");
    Application app(argc, argv);
    QTemporaryDir logs;
    initializeDiagnostics(logs.path(), false);
    Events events;
    setDiagnosticObserver([&](const QJsonObject &event) {
        const QString name = event.value("event").toString();
        const QString message = event.value("message").toString();
        if (name == "plugin-log" && message.endsWith("looping"))
            events.looping = true;
        if (name == "plugin-log" && message.endsWith("closed"))
            events.closed = true;
        if (name == "plugin-log" && message.endsWith("sorting"))
            events.sorting = true;
        if (!name.startsWith("plugin"))
            return;
        QMutexLocker lock(&events.mutex);
        events.lines.append(name + ' ' + event.value("folder").toString() + ' '
                            + event.value("plugin").toString() + ' '
                            + event.value("reason").toString());
    });
    mainthread::Interaction screen([] { return true; });
    mainthread::checkEventsOnly(true);

    discoveryOrder(events);
    folderMustMatchId(events);
    silentFallsThrough();
    noPluginsRepliesNothing();
    folderResolvedOffMainThread();
    skipsWithoutBeforeLookup();
    invalidManifestsSkipped(events);
    threeFailuresDisable(events);
    queuedBehindSlowOpen();
    destroyWhileRunning(events);
    replyOnMainThread();
    storePersistsAcrossBooks();
    sleepFlushes();
    noDataRootStillAnswers(events);
    closeRunsBookClosed(events);
    hungCloseDetaches(events);

    setDiagnosticObserver({});
    std::puts("PASS: plugin host order, fallthrough, discovery, folder resolution, skipping, "
              "failures, queueing, shutdown and reply thread");
}
