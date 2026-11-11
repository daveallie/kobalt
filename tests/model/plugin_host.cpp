#include "plugins/plugin_host.h"

#include "check.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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

std::unique_ptr<PluginHost> open(PluginHost::Directory directory)
{
    std::unique_ptr<PluginHost> host;
    inEvent([&] {
        host.reset(new PluginHost(
                std::move(directory), Context(),
                { QStringLiteral("id"), QStringLiteral("Wolf Hall"), QStringLiteral("en") }));
    });
    return host;
}

std::unique_ptr<PluginHost> open(const QString &directory)
{
    return open([directory] { return directory; });
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
    std::atomic_bool looping{ false };

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
    const QStringList lines = events.take();
    require(lines.filter("plugin-skipped").filter("third").filter("duplicates").size() == 1,
            "A duplicate plugin id was not logged with its folder");
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
        if (name == "plugin-log" && event.value("message").toString().endsWith("looping"))
            events.looping = true;
        if (!name.startsWith("plugin-"))
            return;
        QMutexLocker lock(&events.mutex);
        events.lines.append(name + ' ' + event.value("folder").toString() + ' '
                            + event.value("plugin").toString() + ' '
                            + event.value("reason").toString());
    });
    mainthread::Interaction screen([] { return true; });
    mainthread::checkEventsOnly(true);

    discoveryOrder(events);
    silentFallsThrough();
    noPluginsRepliesNothing();
    folderResolvedOffMainThread();
    skipsWithoutBeforeLookup();
    invalidManifestsSkipped(events);
    threeFailuresDisable(events);
    queuedBehindSlowOpen();
    destroyWhileRunning(events);
    replyOnMainThread();

    setDiagnosticObserver({});
    std::puts("PASS: plugin host order, fallthrough, discovery, folder resolution, skipping, "
              "failures, queueing, shutdown and reply thread");
}
