#include "plugins/plugin_store.h"

#include "check.h"
#include "support/diagnostics.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

using namespace kobalt::plugins;

namespace {
using Scope = PluginStore::Scope;

QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "A store file was not readable");
    return file.readAll();
}

void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(),
            "A fixture file was not written");
}

bool set(PluginStore &store, Scope scope, const QString &key, const QByteArray &json,
         QString *error = nullptr)
{
    QString message;
    const bool ok = store.set(scope, key, json, &message);
    if (error)
        *error = message;
    return ok;
}

void saved(PluginStore &store)
{
    QString error;
    const bool ok = store.save(&error);
    if (!ok)
        std::fprintf(stderr, "%s\n", qPrintable(error));
    require(ok, "The store was not saved");
}

QString bookPath(const QString &root, const QString &key)
{
    return root + "/plugin/" + PluginStore::bookFile(key);
}

void roundTrip()
{
    QTemporaryDir root;
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    require(set(store, Scope::Global, "a", "{\"x\":1}"), "A global value was refused");
    require(set(store, Scope::Book, "b", "\"s\""), "A book value was refused");
    saved(store);
    require(QFile::exists(root.path() + "/plugin/global.json"), "global.json was not written");
    require(QFile::exists(bookPath(root.path(), "book")), "The book file was not written");

    PluginStore again(root.path(), "plugin", "book");
    again.load();
    require(again.value(Scope::Global, "a") == "{\"x\":1}", "The global value did not survive");
    require(again.value(Scope::Book, "b") == "\"s\"", "The book value did not survive");
    require(again.value(Scope::Book, "a").isEmpty(), "The scopes were mixed");

    require(set(again, Scope::Global, "a", {}), "A value was not removed");
    require(again.value(Scope::Global, "a").isEmpty(), "A removed value is still there");
}

void writesOnlyWhenChanged()
{
    QTemporaryDir root;
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    saved(store);
    require(!QFile::exists(root.path() + "/plugin/global.json"),
            "An unchanged empty store wrote a file");
    require(set(store, Scope::Global, "a", "1") && set(store, Scope::Book, "b", "2"),
            "A value was refused");
    saved(store);
    const QString global = root.path() + "/plugin/global.json";
    const QString book = bookPath(root.path(), "book");
    const QDateTime old = QDateTime::fromSecsSinceEpoch(1'000'000'000);
    for (const QString &path : { global, book }) {
        QFile file(path);
        require(file.open(QIODevice::ReadWrite)
                        && file.setFileTime(old, QFileDevice::FileModificationTime),
                "The modification time was not set");
    }
    saved(store);
    require(set(store, Scope::Global, "a", "1"), "An equal value was refused");
    saved(store);
    require(QFileInfo(global).lastModified() == old, "An unchanged global store was rewritten");
    require(QFileInfo(book).lastModified() == old, "An unchanged book store was rewritten");
    require(set(store, Scope::Book, "b", "3"), "A changed value was refused");
    saved(store);
    require(QFileInfo(global).lastModified() == old, "Saving the book rewrote the global store");
    require(QFileInfo(book).lastModified() != old, "A changed book store was not written");
}

void caps()
{
    QTemporaryDir root;
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    // Key "k" is 1 byte and a JSON string adds 2 quotes.
    const auto text = [](qint64 length) { return '"' + QByteArray(int(length), 'a') + '"'; };
    require(set(store, Scope::Global, "k", text(PluginStore::globalCap - 3)),
            "A global value at the cap was refused");
    require(store.size(Scope::Global) == PluginStore::globalCap, "The global size is wrong");
    QString error;
    require(!set(store, Scope::Global, "k", text(PluginStore::globalCap - 2), &error)
                    && error.contains("limit"),
            "A global value past the cap was accepted");
    require(store.value(Scope::Global, "k").size() == PluginStore::globalCap - 1,
            "A refused value replaced the old one");
    require(set(store, Scope::Book, "k", text(PluginStore::bookCap - 3)),
            "A book value at the cap was refused");
    require(!set(store, Scope::Book, "j", "1", &error) && error.contains("limit"),
            "A book value past the cap was accepted");
}

void keyRules()
{
    QTemporaryDir root;
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    require(!set(store, Scope::Global, "", "1"), "An empty key was accepted");
    require(!set(store, Scope::Global, QString(257, 'k'), "1"), "A 257-byte key was accepted");
    require(set(store, Scope::Global, QString(256, 'k'), "1"), "A 256-byte key was refused");
}

void badFileBackedUp()
{
    const QByteArray bad[] = { "{ not json", "{\"format\":2,\"values\":{}}",
                               "{\"format\":1,\"values\":{\"k\":\""
                                       + QByteArray(int(PluginStore::globalCap), 'a') + "\"}}" };
    for (const QByteArray &bytes : bad) {
        QTemporaryDir root;
        write(root.path() + "/plugin/global.json", bytes);
        PluginStore store(root.path(), "plugin", "book");
        store.load();
        require(store.size(Scope::Global) == 0, "A bad store file was loaded");
        require(!QFile::exists(root.path() + "/plugin/global.json"),
                "A bad store file was left in place");
        const QDir backups(root.path() + "/plugin/backups");
        const QStringList files = backups.entryList(QDir::Files);
        require(files.size() == 1, "A bad store file was not moved to backups");
        require(read(backups.filePath(files.first())) == bytes,
                "The backup does not hold the original bytes");
        require(set(store, Scope::Global, "a", "1"), "A store that started empty refused a value");
        saved(store);
        require(backups.entryList(QDir::Files).size() == 1, "Saving changed the backup");
    }
}

// A bad file that cannot be moved aside stays as it is: the store refuses
// writes, so a save never replaces the original.
void unmovableBadFileKept()
{
    QTemporaryDir root;
    const QByteArray bytes = "{ not json";
    write(root.path() + "/plugin/global.json", bytes);
    // A file where the backups folder belongs makes the move fail.
    write(root.path() + "/plugin/backups", "");
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    require(store.size(Scope::Global) == 0, "A bad store file was loaded");
    QString error;
    require(!set(store, Scope::Global, "a", "1", &error) && !error.isEmpty(),
            "A store whose bad file stayed in place accepted a value");
    require(set(store, Scope::Book, "a", "1"), "The other store stopped working");
    saved(store);
    require(read(root.path() + "/plugin/global.json") == bytes,
            "A bad store file that could not be moved was overwritten");
}

void bookKeyed()
{
    const QByteArray hash = QCryptographicHash::hash("k", QCryptographicHash::Sha256).toHex();
    require(PluginStore::bookFile("k") == "books/" + QString::fromLatin1(hash) + ".json",
            "The book file name is wrong");
    require(PluginStore::bookFile("k") != PluginStore::bookFile("l"), "Two books share a file");
}

void noRoot()
{
    PluginStore store({}, "plugin", "book");
    store.load();
    QString error;
    require(!set(store, Scope::Global, "a", "1", &error) && error.contains("no plugin data folder"),
            "A store without a folder accepted a value");
}

void clearRemoves()
{
    QTemporaryDir root;
    PluginStore store(root.path(), "plugin", "book");
    store.load();
    require(set(store, Scope::Global, "a", "1"), "A value was refused");
    saved(store);
    QString error;
    require(store.clear(&error), "The store was not cleared");
    require(!QDir(root.path() + "/plugin").exists(), "The data folder was not removed");
    require(store.value(Scope::Global, "a").isEmpty(), "A cleared value is still there");
    saved(store);
    require(!QDir(root.path() + "/plugin").exists(), "Saving a cleared store wrote files");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir logs;
    kobalt::initializeDiagnostics(logs.path(), false);
    roundTrip();
    writesOnlyWhenChanged();
    caps();
    keyRules();
    badFileBackedUp();
    unmovableBadFileKept();
    bookKeyed();
    noRoot();
    clearRemoves();
    std::puts("PASS: plugin data is kept per plugin and per book, within limits, with backups");
}
