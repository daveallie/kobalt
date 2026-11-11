#include "plugins/lua_plugin.h"

#include "check.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

using namespace kobalt::plugins;

namespace {
QString fixture(const QString &name)
{
    return QStringLiteral(KOBALT_TEST_PLUGIN_FIXTURES "/") + name;
}

Manifest manifest(const QString &directory)
{
    QString error;
    Manifest result;
    const bool ok = readManifest(directory, &result, &error);
    if (!ok)
        std::fprintf(stderr, "%s\n", qPrintable(error));
    require(ok, "A fixture manifest did not load");
    return result;
}

Book book()
{
    return { QStringLiteral("id"), QStringLiteral("Wolf Hall"), QStringLiteral("en") };
}

Lookup lookup(const QString &word)
{
    Lookup result;
    result.epoch = 7;
    result.word = word;
    result.position = { 41.5, 12, 300, QStringLiteral("Part One") };
    return result;
}

// Runs before_lookup; `ok` and `error` describe the call.
Card ask(LuaPlugin &plugin, const QString &word, bool *ok, QString *error = nullptr)
{
    Card card;
    QString message;
    *ok = plugin.beforeLookup(lookup(word), book(), &card, &message);
    if (error)
        *error = message;
    return card;
}

QString askHtml(LuaPlugin &plugin, const QString &word)
{
    bool ok = false;
    QString error;
    const auto card = ask(plugin, word, &ok, &error);
    if (!ok)
        std::fprintf(stderr, "%s\n", qPrintable(error));
    require(ok && !card.isEmpty(), "before_lookup did not return a card");
    return card.html;
}

void loaded(LuaPlugin &plugin)
{
    QString error;
    const bool ok = plugin.load(&error);
    if (!ok)
        std::fprintf(stderr, "%s\n", qPrintable(error));
    require(ok, "main.lua did not run");
}

void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(),
            "A fixture file was not written");
}

void sandboxLibraries()
{
    LuaPlugin plugin(manifest(fixture("sandbox")), {});
    loaded(plugin);
    require(askHtml(plugin, "word") == "ok", "The sandbox fixture did not answer");
}

void readFileStaysInFolder()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "No temporary folder");
    const QString folder = temporary.path() + "/plugin";
    require(QDir().mkpath(folder), "No plugin folder");
    for (const char *name : { "plugin.json", "main.lua", "data.txt" })
        require(QFile::copy(fixture("read_file/") + name, folder + "/" + QString(name)),
                "The fixture was not copied");
    write(temporary.path() + "/x", "outside");
    write(temporary.path() + "/plugin-sibling.txt", "sibling");
    require(QFile::link(temporary.path() + "/x", folder + "/escape.txt"), "No symlink");

    LuaPlugin plugin(manifest(folder), {});
    loaded(plugin);
    require(askHtml(plugin, "data.txt") == "Plugin data\n", "read_file did not read a plugin file");
    require(askHtml(plugin, "../x") == "<nil>", "read_file left the folder through ..");
    require(askHtml(plugin, "../plugin-sibling.txt") == "<nil>",
            "read_file accepted a folder with the same prefix");
    require(askHtml(plugin, temporary.path() + "/x") == "<nil>",
            "read_file accepted an absolute path");
    require(askHtml(plugin, "escape.txt") == "<nil>", "read_file followed a symlink out");
    require(askHtml(plugin, "missing.txt") == "<nil>", "read_file returned a missing file");
    require(askHtml(plugin, ".") == "<nil>", "read_file returned the folder itself");
}

void readSidecar()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "No temporary folder");
    write(temporary.path() + "/Book.xray.json", "{\"format\": \"kobalt-xray/1\"}");
    write(temporary.path() + "/Other.kepub.xray.json", "kepub");
    write(temporary.path() + "/secret.json", "secret");
    Context context{ temporary.path() + "/Book.epub", true };

    LuaPlugin plugin(manifest(fixture("sidecar")), context);
    loaded(plugin);
    require(askHtml(plugin, "xray.json") == "{\"format\": \"kobalt-xray/1\"}",
            "read_sidecar did not read the sidecar");
    require(askHtml(plugin, "missing.json") == "<nil>", "A missing sidecar was not nil");
    require(askHtml(plugin, "x/../../secret.json") == "<nil>", "A suffix left the book folder");
    require(askHtml(plugin, "..json") == "<nil>", "A suffix with .. was accepted");
    require(askHtml(plugin, "x\\y") == "<nil>", "A suffix with a backslash was accepted");

    LuaPlugin kepub(manifest(fixture("sidecar")), { temporary.path() + "/Other.kepub.epub", true });
    loaded(kepub);
    require(askHtml(kepub, "xray.json") == "kepub", "Only the final extension is replaced");

    LuaPlugin disabled(manifest(fixture("sidecar")), { context.bookFile, false });
    loaded(disabled);
    require(askHtml(disabled, "xray.json") == "<nil>", "Sidecars were read while disabled");

    LuaPlugin bookless(manifest(fixture("sidecar")), {});
    loaded(bookless);
    require(askHtml(bookless, "xray.json") == "<nil>", "A sidecar was read without a book");
}

void jsonDecode()
{
    LuaPlugin plugin(manifest(fixture("json")), {});
    loaded(plugin);
    require(askHtml(plugin, "word") == "ok", "The JSON fixture did not answer");
}

void budgetAbort()
{
    LuaPlugin plugin(manifest(fixture("budget")), {});
    loaded(plugin);
    QElapsedTimer timer;
    timer.start();
    bool ok = true;
    QString error;
    const auto card = ask(plugin, "word", &ok, &error);
    require(!ok && card.isEmpty(), "An endless loop did not fail");
    require(timer.elapsed() < 1000, "The budget did not stop the loop within a second");
    require(error.contains("budget"), "The error does not mention the budget");

    // The budget is per call, so a later call can still run.
    LuaPlugin small(manifest(fixture("budget")), {}, 10'000);
    loaded(small);
    ask(small, "word", &ok, &error);
    require(!ok && error.contains("budget"), "A small budget did not stop the loop");
}

void budgetCoversLoad()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "No temporary folder");
    require(QFile::copy(fixture("budget/plugin.json"), temporary.path() + "/plugin.json"),
            "The manifest was not copied");
    write(temporary.path() + "/main.lua",
          "while true do pcall(function() while true do end end) end\n");
    LuaPlugin plugin(manifest(temporary.path()), {});
    QString error;
    require(!plugin.load(&error), "An endless main.lua loaded");
    require(error.contains("budget"), "The load error does not mention the budget");
}

void errorIsFailure()
{
    LuaPlugin plugin(manifest(fixture("error")), {});
    loaded(plugin);
    bool ok = true;
    QString error;
    const auto card = ask(plugin, "word", &ok, &error);
    require(!ok && card.isEmpty(), "A Lua error was not a failure");
    require(error.contains("x marks the failure"), "The error lost the Lua message");
    require(error.contains("plugin error"), "The error does not name the plugin");
}

void malformedCardIsFailure()
{
    LuaPlugin plugin(manifest(fixture("malformed")), {});
    loaded(plugin);
    for (const char *reply : { "text", "empty", "untitled", "blank", "number" }) {
        bool ok = true;
        QString error;
        const auto card = ask(plugin, reply, &ok, &error);
        if (ok)
            std::fprintf(stderr, "Reply %s was accepted\n", reply);
        require(!ok && card.isEmpty() && !error.isEmpty(), "A malformed card was not a failure");
    }
}

void nilIsNoCard()
{
    LuaPlugin plugin(manifest(fixture("nil_card")), {});
    loaded(plugin);
    bool ok = false;
    const auto card = ask(plugin, "word", &ok);
    require(ok && card.isEmpty(), "nil was not a successful call without a card");
}

void missingHooksSucceed()
{
    LuaPlugin plugin(manifest(fixture("nil_card")), {});
    loaded(plugin);
    QString error;
    require(plugin.bookOpened(book(), &error), "A missing book_opened failed");
    require(plugin.bookClosed(&error), "A missing book_closed failed");

    LuaPlugin unloaded(manifest(fixture("nil_card")), {});
    bool ok = true;
    ask(unloaded, "word", &ok, &error);
    require(!ok, "A plugin that did not load answered");
}

void utf8RoundTrip()
{
    LuaPlugin plugin(manifest(fixture("echo")), {});
    loaded(plugin);
    const QString word = QString::fromUtf8("Zo\xc3\xab\xe2\x80\x99s");
    bool ok = false;
    const auto card = ask(plugin, word, &ok);
    require(ok && card.title == word, "The word did not round-trip through Lua");
    require(card.footer == "echo", "The footer was lost");
}

void ctxShape()
{
    LuaPlugin plugin(manifest(fixture("echo")), {});
    loaded(plugin);
    require(askHtml(plugin, "word") == "Wolf Hall|en|41.5|12|300|Part One|nil",
            "Lua did not see the lookup context");
    QString error;
    require(plugin.bookOpened(book(), &error), "book_opened failed");
    require(askHtml(plugin, "word").endsWith("|Wolf Hall"), "book_opened did not see the book");
    require(plugin.bookClosed(&error), "book_closed failed");
    require(askHtml(plugin, "word").endsWith("|nil"), "book_closed did not run");
}

void memoryCap()
{
    require(defaultMemoryCap == 32 * 1024 * 1024, "The default memory cap is not 32 MiB");
    LuaPlugin plugin(manifest(fixture("memory_cap")), {}, defaultInstructionBudget, 1024 * 1024);
    loaded(plugin);
    bool ok = true;
    QString error;
    const auto card = ask(plugin, "grow", &ok, &error);
    require(!ok && card.isEmpty(), "Growing past the memory cap did not fail");
    require(error.contains("not enough memory"), "The error does not report the memory limit");
    require(askHtml(plugin, "word") == "ok", "The plugin did not recover after the memory error");
    require(askHtml(plugin, "json").contains("memory limit"),
            "Large JSON was decoded past the memory cap");
}

void finalizerRejected()
{
    {
        LuaPlugin plugin(manifest(fixture("finalizer")), {});
        loaded(plugin);
        require(askHtml(plugin, "word") == "indexed", "An ordinary metatable was rejected");
        bool ok = true;
        QString error;
        const auto card = ask(plugin, "finalizer", &ok, &error);
        require(!ok && card.isEmpty(), "A __gc metatable was accepted");
        require(error.contains("__gc"), "The error does not name __gc");
        require(askHtml(plugin, "word") == "indexed", "The plugin did not recover");
    }
    puts("Destroying a plugin after a rejected finalizer returns");
}

void patternSteps()
{
    LuaPlugin plugin(manifest(fixture("pattern")), {});
    loaded(plugin);
    require(askHtml(plugin, "word") == "200000", "An ordinary long gsub failed");
    QElapsedTimer timer;
    timer.start();
    bool ok = true;
    QString error;
    const auto card = ask(plugin, "slow", &ok, &error);
    require(!ok && card.isEmpty(), "A backtracking pattern did not fail");
    require(error.contains("pattern too complex"), "The error does not report the pattern");
    require(timer.elapsed() < 1000, "The pattern limit did not stop the match within a second");

    // Each start position stays cheap; only a budget for the whole call stops this.
    timer.restart();
    ask(plugin, "unanchored", &ok, &error);
    require(!ok && error.contains("pattern too complex"),
            "An unanchored scan of a long subject did not fail");
    require(timer.elapsed() < 1000, "The unanchored scan was not stopped within a second");
}

void manifestValidation()
{
    const auto valid = manifest(fixture("echo"));
    require(valid.id == "echo" && valid.name == "echo" && valid.version == "1" && valid.api == 1,
            "The manifest fields are wrong");
    require(QDir(valid.directory) == QDir(fixture("echo")), "The manifest folder is wrong");
    for (const char *name :
         { "api_two", "missing_id", "bad_id", "invalid_json", "missing_main", "absent" }) {
        QString error;
        Manifest result;
        const bool ok = readManifest(fixture(name), &result, &error);
        if (ok)
            std::fprintf(stderr, "Manifest %s was accepted\n", name);
        require(!ok && !error.isEmpty(), "An invalid plugin was accepted");
    }
}
} // namespace

int main()
{
    sandboxLibraries();
    readFileStaysInFolder();
    readSidecar();
    jsonDecode();
    budgetAbort();
    budgetCoversLoad();
    errorIsFailure();
    malformedCardIsFailure();
    nilIsNoCard();
    missingHooksSucceed();
    utf8RoundTrip();
    ctxShape();
    memoryCap();
    finalizerRejected();
    patternSteps();
    manifestValidation();
    puts("PASS: Lua plugins run in a sandbox");
}
