#include "plugins/plugin_host.h"

#include "check.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
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

std::atomic_int statusEvents{ 0 };

std::unique_ptr<PluginHost> open(PluginHost::Directory directory, int joinLimitMs = 2000,
                                 QString dataRoot = {}, QString bookKey = QStringLiteral("key"),
                                 Switches switches = {})
{
    std::unique_ptr<PluginHost> host;
    inEvent([&] {
        host.reset(new PluginHost(
                std::move(directory), [dataRoot] { return dataRoot; }, Context(),
                { QStringLiteral("id"), QStringLiteral("Wolf Hall"), QStringLiteral("en"),
                  bookKey },
                switches, joinLimitMs));
        host->statusChanged = [](const QVector<PluginStatus> &) { ++statusEvents; };
    });
    return host;
}

std::unique_ptr<PluginHost> open(const QString &directory, int joinLimitMs = 2000)
{
    return open([directory] { return directory; }, joinLimitMs);
}

std::unique_ptr<PluginHost> openWithData(const QString &directory, const QString &dataRoot,
                                         const QString &bookKey = QStringLiteral("key"),
                                         Switches switches = {})
{
    return open([directory] { return directory; }, 2000, dataRoot, bookKey, switches);
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
    QVector<Card> cards;
    QVector<qint64> arrivals;
    bool mainThread = false;
    QElapsedTimer clock;

    // The first card, or an empty one.
    Card card() const { return cards.isEmpty() ? Card() : cards.first(); }
};

std::shared_ptr<Answer> ask(PluginHost &host, const QString &word, quint64 epoch = 1)
{
    auto answer = std::make_shared<Answer>();
    answer->clock.start();
    inEvent([&] {
        Lookup lookup;
        lookup.epoch = epoch;
        lookup.word = word;
        host.lookup(
                lookup,
                [answer](quint64 replyEpoch, Card card) {
                    require(!answer->replied, "A card arrived after the lookup was done");
                    require(replyEpoch == answer->epoch || answer->cards.isEmpty(),
                            "Cards of one lookup carried different epochs");
                    answer->epoch = replyEpoch;
                    answer->cards.append(card);
                    answer->arrivals.append(answer->clock.elapsed());
                },
                [answer](quint64 replyEpoch) {
                    require(!answer->replied, "A lookup finished twice");
                    answer->replied = true;
                    answer->epoch = replyEpoch;
                    answer->mainThread = QThread::currentThread() == qApp->thread();
                });
    });
    return answer;
}

Card answered(PluginHost &host, const QString &word = QStringLiteral("Cromwell"))
{
    const auto answer = ask(host, word);
    require(waitFor([&] { return answer->replied; }), "A lookup never replied");
    return answer->card();
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
        require(answer->cards.isEmpty() && answer->epoch == 9,
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
    require(waitFor([&] {
                const auto statuses = host->statuses();
                return statuses.size() == 1 && statuses.first().state == PluginStatus::Stopped;
            }),
            "A plugin stopped after three failures was not reported as stopped");
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
    require(next->pluginsBlocked() && next->statuses().isEmpty(),
            "A host opened beside a hung thread did not report that plugins are blocked");
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

void cardsArriveSeparately()
{
    auto host = open(fixture("pair"));
    const auto answer = ask(*host, QStringLiteral("Cromwell"), 3);
    require(waitFor([&] { return answer->replied; }), "A lookup never finished");
    require(answer->cards.size() == 2, "Each answering plugin did not add its own card");
    require(answer->cards[0].pluginId == "a_fast" && answer->cards[0].pluginName == "Fast"
                    && answer->cards[1].pluginId == "b_slow" && answer->cards[1].title == "slow",
            "Cards did not carry their plugin in id order");
    require(answer->arrivals[1] - answer->arrivals[0] >= 100,
            "The fast card waited for the slow plugin");
    require(answer->epoch == 3, "The cards lost their epoch");
    close(host);
}

void staleLookupSkipped()
{
    auto host = open(fixture("stale"));
    const auto first = ask(*host, QStringLiteral("first"), 1);
    const auto second = ask(*host, QStringLiteral("second"), 2);
    require(waitFor([&] { return first->replied && second->replied; }),
            "A queued lookup never finished");
    require(first->cards.isEmpty() && first->epoch == 1,
            "A lookup that was already stale ran the plugins");
    require(second->card().title == "second", "The newest lookup did not run");
    close(host);
}

PluginStatus statusOf(const QVector<PluginStatus> &statuses, const QString &id)
{
    for (const PluginStatus &status : statuses)
        if (status.id == id)
            return status;
    std::fprintf(stderr, "No status for %s\n", qPrintable(id));
    require(false, "A plugin had no status");
    return {};
}

void statusSnapshot()
{
    QTemporaryDir data;
    QDir(data.path()).mkpath("ghost");
    QDir(data.path()).mkpath("ok");
    Switches switches;
    switches.disabled.insert(QStringLiteral("off"));
    const int before = statusEvents;
    auto host = openWithData(fixture("statuses"), data.path(), QStringLiteral("key"), switches);
    require(waitFor([&] { return host->statuses().size() == 6; }),
            "The plugin statuses were not published");
    require(statusEvents > before, "statusChanged did not fire");
    const auto statuses = host->statuses();
    const auto ok = statusOf(statuses, "ok");
    require(ok.state == PluginStatus::Running && ok.name == "Okay" && ok.version == "2.1"
                    && ok.description == "Answers every word." && ok.folder.endsWith("/ok"),
            "A running plugin's status is wrong");
    require(statusOf(statuses, "off").state == PluginStatus::TurnedOff,
            "A turned-off plugin was not reported as turned off");
    const auto noMain = statusOf(statuses, "no_main");
    require(noMain.state == PluginStatus::NotLoaded && noMain.reason.contains("main.lua"),
            "A plugin without main.lua was not reported");
    const auto mismatch = statusOf(statuses, "mismatch_dir");
    require(mismatch.state == PluginStatus::NotLoaded
                    && mismatch.reason.contains("folder name must match id"),
            "A folder named differently from its id was not reported");
    require(statusOf(statuses, "load_error").state == PluginStatus::NotLoaded,
            "A plugin that failed to load was not reported");
    require(statusOf(statuses, "ghost").state == PluginStatus::DataOnly,
            "Data without a plugin was not reported");
    require(answered(*host).title == "ok", "The running plugin did not answer");
    close(host);
}

void safeModeLoadsNothing()
{
    Switches switches;
    switches.enabled = false;
    auto host = openWithData(fixture("statuses"), QString(), QStringLiteral("key"), switches);
    require(waitFor([&] { return host->statuses().size() == 5; }),
            "Statuses were not published with plugins off");
    require(host->skipsLookups(), "A lookup still waited with plugins off");
    for (const char *id : { "ok", "off" })
        require(statusOf(host->statuses(), id).state == PluginStatus::TurnedOff,
                "A plugin was not turned off with plugins off");
    require(answered(*host).isEmpty(), "A plugin answered with plugins off");
    close(host);
}

void deleteDataClears()
{
    QTemporaryDir data;
    auto host = openWithData(fixture("remember"), data.path());
    require(answered(*host).title == "book 1 global 1", "The lookup was not counted");
    inEvent([&] { host->flush(); });
    require(waitFor([&] { return QFile::exists(data.path() + "/remember/global.json"); }),
            "The data was not written");
    bool done = false, deleted = false;
    inEvent([&] {
        host->deleteData(QStringLiteral("remember"), [&](bool ok, const QString &) {
            done = true;
            deleted = ok;
        });
    });
    require(waitFor([&] { return done; }) && deleted, "Deleting plugin data failed");
    require(!QDir(data.path() + "/remember").exists(), "The data folder was not removed");
    require(answered(*host).title == "book 1 global 1", "The running plugin kept its data");
    bool refused = false;
    done = false;
    inEvent([&] {
        host->deleteData(QStringLiteral(".."), [&](bool ok, const QString &) {
            done = true;
            refused = !ok;
        });
    });
    require(waitFor([&] { return done; }) && refused && QDir(data.path()).exists(),
            "Deleting a path outside the data folder was not refused");
    close(host);
}

void replyOnMainThread()
{
    auto host = open(fixture("single"));
    const auto answer = ask(*host, QStringLiteral("Cromwell"), 42);
    require(waitFor([&] { return answer->replied; }), "A lookup never replied");
    require(answer->mainThread, "A reply ran off the main thread");
    require(answer->epoch == 42 && answer->card().title == "Cromwell",
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
    cardsArriveSeparately();
    staleLookupSkipped();
    statusSnapshot();
    safeModeLoadsNothing();
    deleteDataClears();
    closeRunsBookClosed(events);
    hungCloseDetaches(events);

    setDiagnosticObserver({});
    std::puts("PASS: plugin host order, fallthrough, discovery, folder resolution, skipping, "
              "failures, queueing, shutdown and reply thread");
}
