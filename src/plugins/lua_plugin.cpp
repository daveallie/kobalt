#include "lua_plugin.h"

#include "support/diagnostics.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <cmath>
#include <cstdlib>
#include <lua.hpp>

namespace kobalt::plugins {
namespace {
constexpr int hookInterval = 1000;
constexpr int logLimit = 1000;
constexpr int logLines = 20;
constexpr qint64 sourceLimit = 1024 * 1024;
constexpr int titleLimit = 200;
constexpr std::size_t htmlLimit = 64 * 1024;
constexpr std::size_t stoppedGrowth = 64 * 1024;
// Registry keys of the hook's error messages; mutable so that they never share an address.
char stoppedKey, budgetKey;

QString text(lua_State *state, int index)
{
    size_t length = 0;
    const char *data = lua_tolstring(state, index, &length);
    return QString::fromUtf8(data, int(length));
}

void push(lua_State *state, const QString &value)
{
    const QByteArray data = value.toUtf8();
    lua_pushlstring(state, data.constData(), size_t(data.size()));
}

void setField(lua_State *state, const char *key, const QString &value)
{
    push(state, value);
    lua_setfield(state, -2, key);
}

void pushBook(lua_State *state, const Book &book)
{
    lua_createtable(state, 0, 2);
    setField(state, "title", book.title);
    setField(state, "language", book.language);
}

enum class Read { Ok, Missing, TooLarge };

// Checks the size first, so that a huge file is never read.
Read readBytes(const QString &path, qint64 limit, QByteArray *data)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Read::Missing;
    const qint64 size = file.size();
    if (size > limit)
        return Read::TooLarge;
    *data = file.read(size);
    return Read::Ok;
}

using Memory = LuaPlugin::Memory;

bool pushJson(lua_State *state, const QJsonValue &value, int depth, const Memory &memory);

bool pushJsonObject(lua_State *state, const QJsonObject &object, int depth, const Memory &memory)
{
    lua_createtable(state, 0, int(object.size()));
    for (auto it = object.begin(); it != object.end(); ++it) {
        push(state, it.key());
        if (!pushJson(state, it.value(), depth + 1, memory))
            return false;
        lua_rawset(state, -3);
    }
    return true;
}

bool pushJsonArray(lua_State *state, const QJsonArray &array, int depth, const Memory &memory)
{
    lua_createtable(state, int(array.size()), 0);
    for (int i = 0; i < array.size(); ++i) {
        if (!pushJson(state, array.at(i), depth + 1, memory))
            return false;
        lua_rawseti(state, -2, i + 1);
    }
    return true;
}

// Stops as soon as the tables pass the cap, because their size is not the text's.
bool pushJson(lua_State *state, const QJsonValue &value, int depth, const Memory &memory)
{
    if (memory.used > memory.cap || !lua_checkstack(state, 4) || depth > 512)
        return false;
    switch (value.type()) {
    case QJsonValue::Bool:
        lua_pushboolean(state, value.toBool());
        return true;
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (std::floor(number) == number && std::fabs(number) < 9007199254740992.0)
            lua_pushinteger(state, lua_Integer(number));
        else
            lua_pushnumber(state, number);
        return true;
    }
    case QJsonValue::String:
        push(state, value.toString());
        return true;
    case QJsonValue::Array:
        return pushJsonArray(state, value.toArray(), depth, memory);
    case QJsonValue::Object:
        return pushJsonObject(state, value.toObject(), depth, memory);
    default:
        lua_pushnil(state);
        return true;
    }
}

// load() that accepts only text chunks; the original is upvalue 1.
int textLoad(lua_State *state)
{
    if (!lua_isnoneornil(state, 3)) {
        const char *mode = lua_tostring(state, 3);
        if (!mode || qstrcmp(mode, "t") != 0) {
            lua_pushnil(state);
            lua_pushliteral(state, "only text chunks can be loaded");
            return 2;
        }
    }
    if (lua_gettop(state) < 3)
        lua_settop(state, 3);
    lua_pushliteral(state, "t");
    lua_replace(state, 3);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
    return lua_gettop(state);
}

// Lua runs finalizers with hooks off, so a __gc could loop past the budget.
int safeSetmetatable(lua_State *state)
{
    if (lua_type(state, 2) == LUA_TTABLE) {
        lua_pushliteral(state, "__gc");
        const bool finalizer = lua_rawget(state, 2) != LUA_TNIL;
        lua_pop(state, 1);
        if (finalizer)
            return luaL_error(state, "metatables with __gc are not allowed");
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
    return lua_gettop(state);
}

void openLibraries(lua_State *state)
{
    const luaL_Reg libraries[] = { { LUA_GNAME, luaopen_base },
                                   { LUA_STRLIBNAME, luaopen_string },
                                   { LUA_TABLIBNAME, luaopen_table },
                                   { LUA_MATHLIBNAME, luaopen_math },
                                   { LUA_UTF8LIBNAME, luaopen_utf8 } };
    for (const luaL_Reg &library : libraries) {
        luaL_requiref(state, library.name, library.func, 1);
        lua_pop(state, 1);
    }
    for (const char *name :
         { "dofile", "loadfile", "require", "print", "io", "os", "package", "debug" }) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
    lua_getglobal(state, "load");
    lua_pushcclosure(state, textLoad, 1);
    lua_setglobal(state, "load");
    lua_getglobal(state, "setmetatable");
    lua_pushcclosure(state, safeSetmetatable, 1);
    lua_setglobal(state, "setmetatable");
}
} // namespace

bool readManifest(const QString &directory, Manifest *out, QString *error)
{
    const QDir folder(directory);
    const QString manifestPath = folder.filePath(QStringLiteral("plugin.json"));
    QByteArray data;
    const Read read = readBytes(manifestPath, sourceLimit, &data);
    if (read != Read::Ok) {
        *error = (read == Read::TooLarge ? QStringLiteral("too large: ")
                                         : QStringLiteral("cannot read "))
                + manifestPath;
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error = manifestPath + QStringLiteral(" is not a JSON object: ") + parse.errorString();
        return false;
    }
    const QJsonObject object = document.object();
    Manifest manifest;
    manifest.id = object.value(QStringLiteral("id")).toString();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]+\\z")).match(manifest.id).hasMatch()
        || manifest.id == QLatin1String(".") || manifest.id == QLatin1String("..")) {
        *error = manifestPath + QStringLiteral(" has no id of letters, digits, '.', '_' or '-'");
        return false;
    }
    const QJsonValue api = object.value(QStringLiteral("api"));
    if (!api.isDouble() || api.toDouble() != 1) {
        *error = QStringLiteral("plugin ") + manifest.id
                + QStringLiteral(" needs an unsupported API version");
        return false;
    }
    manifest.api = 1;
    manifest.name = object.value(QStringLiteral("name")).toString(manifest.id);
    manifest.version = object.value(QStringLiteral("version")).toString();
    manifest.directory = folder.absolutePath();
    if (!QFileInfo(folder.filePath(QStringLiteral("main.lua"))).isFile()) {
        *error = QStringLiteral("plugin ") + manifest.id + QStringLiteral(" has no main.lua");
        return false;
    }
    *out = manifest;
    return true;
}

LuaPlugin::LuaPlugin(Manifest manifest, Context context, int instructionBudget,
                     std::size_t memoryCap)
    : pluginManifest(std::move(manifest)),
      context(std::move(context)),
      root(QFileInfo(pluginManifest.directory).canonicalFilePath()),
      tickLimit(qMax(1, instructionBudget / hookInterval))
{
    memory.cap = memoryCap;
}

LuaPlugin::~LuaPlugin()
{
    if (state)
        lua_close(state);
}

const Manifest &LuaPlugin::manifest() const
{
    return pluginManifest;
}

void LuaPlugin::setAbort(const std::atomic_bool *value)
{
    abort = value;
    memory.abort = value;
}

QString LuaPlugin::failure(const QString &message) const
{
    return QStringLiteral("plugin ") + pluginManifest.id + QStringLiteral(": ") + message;
}

LuaPlugin *LuaPlugin::owner(lua_State *state)
{
    return *static_cast<LuaPlugin **>(lua_getextraspace(state));
}

// The cap applies only while plugin code runs, so host pushes outside lua_pcall cannot panic.
void *LuaPlugin::allocate(void *memory, void *block, std::size_t oldSize, std::size_t newSize)
{
    auto *usage = static_cast<Memory *>(memory);
    if (!block)
        oldSize = 0;
    if (newSize == 0) {
        std::free(block);
        usage->used -= oldSize;
        return nullptr;
    }
    // One instruction can copy up to the cap, so a stopped plugin may not grow by much.
    // Small growth stays, because raising the stop error and unwinding allocate.
    if (usage->enforce && newSize > oldSize
        && (usage->used - oldSize + newSize > usage->cap
            || (newSize - oldSize > stoppedGrowth && usage->abort
                && usage->abort->load(std::memory_order_relaxed)))) {
        ++usage->refused;
        return nullptr;
    }
    void *result = std::realloc(block, newSize);
    if (result)
        usage->used = usage->used - oldSize + newSize;
    return result;
}

std::size_t LuaPlugin::memoryLeft(lua_State *state)
{
    const Memory &usage = owner(state)->memory;
    return usage.used < usage.cap ? usage.cap - usage.used : 0;
}

LuaPlugin::Unenforced::Unenforced(lua_State *state)
    : memory(owner(state)->memory), enforced(memory.enforce)
{
    memory.enforce = false;
}

LuaPlugin::Unenforced::~Unenforced()
{
    memory.enforce = enforced;
}

int LuaPlugin::pushFile(lua_State *state, const QString &path)
{
    const Unenforced unenforced(state);
    const std::size_t left = memoryLeft(state);
    QByteArray data;
    const Read read = readBytes(path, left > 1024 ? qint64(left - 1024) : 0, &data);
    if (read == Read::Missing) {
        lua_pushnil(state);
        return 1;
    }
    if (read == Read::TooLarge) {
        lua_pushnil(state);
        lua_pushliteral(state, "the file exceeds the plugin memory limit");
        return 2;
    }
    lua_pushlstring(state, data.constData(), size_t(data.size()));
    return 1;
}

int LuaPlugin::jsonDecode(lua_State *state)
{
    size_t length = 0;
    const char *data = luaL_checklstring(state, 1, &length);
    const Unenforced unenforced(state);
    Memory &usage = owner(state)->memory;
    if (length > usage.cap / 8 || length * 4 > memoryLeft(state)) {
        lua_pushnil(state);
        lua_pushliteral(state, "JSON exceeds the plugin memory limit");
        return 2;
    }
    bool decoded = false;
    {
        QJsonParseError parse;
        const QJsonDocument document =
                QJsonDocument::fromJson(QByteArray(data, int(length)), &parse);
        lua_settop(state, 0);
        if (parse.error != QJsonParseError::NoError) {
            lua_pushnil(state);
            push(state,
                 parse.errorString() + QStringLiteral(" at offset ")
                         + QString::number(parse.offset));
            return 2;
        }
        if (!document.isArray() && !document.isObject()) {
            lua_pushnil(state);
            lua_pushliteral(state, "JSON must be an object or an array");
            return 2;
        }
        decoded = document.isArray() ? pushJsonArray(state, document.array(), 0, usage)
                                     : pushJsonObject(state, document.object(), 0, usage);
    }
    const bool fits = usage.used <= usage.cap;
    if (!decoded || !fits) {
        lua_settop(state, 0);
        lua_gc(state, LUA_GCCOLLECT);
        lua_pushnil(state);
        if (fits)
            lua_pushliteral(state, "JSON is nested too deeply");
        else
            lua_pushliteral(state, "JSON exceeds the plugin memory limit");
        return 2;
    }
    lua_settop(state, 1);
    return 1;
}

void LuaPlugin::countHook(lua_State *state, lua_Debug *)
{
    LuaPlugin *plugin = owner(state);
    const bool stopped = plugin->abort && plugin->abort->load(std::memory_order_relaxed);
    if (!stopped && ++plugin->ticks <= plugin->tickLimit)
        return;
    // Fail at every instruction from now on, so that pcall cannot resume the loop.
    lua_sethook(state, countHook, LUA_MASKCOUNT, 1);
    // The messages are made in advance: at the cap, each refused allocation runs a full GC.
    lua_rawgetp(state, LUA_REGISTRYINDEX, stopped ? &stoppedKey : &budgetKey);
    lua_error(state);
}

int LuaPlugin::readFile(lua_State *state)
{
    luaL_checkstring(state, 1);
    const LuaPlugin *plugin = owner(state);
    const QString relative = text(state, 1);
    lua_settop(state, 0);
    QString path;
    if (!plugin->root.isEmpty() && !relative.isEmpty() && !QDir::isAbsolutePath(relative)) {
        const QFileInfo info(QDir(plugin->root).filePath(relative));
        const QString canonical = info.canonicalFilePath();
        if (info.isFile() && canonical.startsWith(plugin->root + QLatin1Char('/')))
            path = canonical;
    }
    if (path.isEmpty()) {
        lua_pushnil(state);
        return 1;
    }
    return pushFile(state, path);
}

int LuaPlugin::readSidecar(lua_State *state)
{
    luaL_checkstring(state, 1);
    const LuaPlugin *plugin = owner(state);
    const QString suffix = text(state, 1);
    lua_settop(state, 0);
    QString path;
    if (plugin->context.sidecars && !plugin->context.bookFile.isEmpty() && !suffix.isEmpty()
        && !suffix.contains(QLatin1Char('/')) && !suffix.contains(QLatin1Char('\\'))
        && !suffix.contains(QStringLiteral(".."))) {
        const QFileInfo book(plugin->context.bookFile);
        path = book.path() + QLatin1Char('/') + book.completeBaseName() + QLatin1Char('.') + suffix;
    }
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        lua_pushnil(state);
        return 1;
    }
    return pushFile(state, path);
}

int LuaPlugin::log(lua_State *state)
{
    luaL_checkstring(state, 1);
    LuaPlugin *plugin = owner(state);
    if (++plugin->logs > logLines + 1)
        return 0;
    const QString prefix = QStringLiteral("plugin:") + plugin->pluginManifest.id + QLatin1Char(' ');
    const QString message = plugin->logs > logLines
            ? prefix + QStringLiteral("further log lines suppressed")
            : prefix + text(state, 1).left(logLimit);
    diagnostic("plugin-log", { { "message", message } }, DiagnosticSeverity::Activity);
    return 0;
}

bool LuaPlugin::load(QString *error)
{
    if (state) {
        *error = failure(QStringLiteral("is already loaded"));
        return false;
    }
    QByteArray source;
    const Read read = readBytes(QDir(pluginManifest.directory).filePath(QStringLiteral("main.lua")),
                                sourceLimit, &source);
    if (read != Read::Ok) {
        *error = failure(read == Read::TooLarge ? QStringLiteral("main.lua exceeds 1 MiB")
                                                : QStringLiteral("cannot read main.lua"));
        return false;
    }
    if (root.isEmpty()) {
        *error = failure(QStringLiteral("its folder is missing"));
        return false;
    }
    memory.used = 0;
    memory.enforce = false;
    state = lua_newstate(allocate, &memory);
    if (!state) {
        *error = failure(QStringLiteral("cannot create a Lua state"));
        return false;
    }
    *static_cast<LuaPlugin **>(lua_getextraspace(state)) = this;
    openLibraries(state);
    lua_pushliteral(state, "was stopped");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &stoppedKey);
    lua_pushliteral(state, "exceeded its instruction budget");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &budgetKey);

    lua_createtable(state, 0, 4);
    lua_createtable(state, 0, 1);
    lua_pushcfunction(state, jsonDecode);
    lua_setfield(state, -2, "decode");
    lua_setfield(state, -2, "json");
    lua_pushcfunction(state, readSidecar);
    lua_setfield(state, -2, "read_sidecar");
    lua_pushcfunction(state, readFile);
    lua_setfield(state, -2, "read_file");
    lua_pushcfunction(state, log);
    lua_setfield(state, -2, "log");
    lua_setglobal(state, "kobalt");

    if (luaL_loadbufferx(state, source.constData(), size_t(source.size()), "@main.lua", "t")
        != LUA_OK) {
        *error = failure(text(state, -1));
        lua_close(state);
        state = nullptr;
        return false;
    }
    if (!call(0, 0, error)) {
        lua_close(state);
        state = nullptr;
        return false;
    }
    return true;
}

// Raw reads keep plugin metamethods from running outside the protected call.
bool LuaPlugin::pushHook(const char *name)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushstring(state, name);
    const bool found = lua_rawget(state, -2) == LUA_TFUNCTION;
    lua_remove(state, -2);
    if (!found)
        lua_pop(state, 1);
    return found;
}

bool LuaPlugin::answersLookups()
{
    if (!state || !pushHook("before_lookup"))
        return false;
    lua_pop(state, 1);
    return true;
}

bool LuaPlugin::call(int arguments, int results, QString *error)
{
    ticks = 0;
    logs = 0;
    lua_sethook(state, countHook, LUA_MASKCOUNT, hookInterval);
    memory.enforce = true;
    const int status = lua_pcall(state, arguments, results, 0);
    memory.enforce = false;
    lua_sethook(state, nullptr, 0, 0);
    if (status == LUA_OK)
        return true;
    *error = failure(lua_type(state, -1) == LUA_TSTRING
                             ? text(state, -1)
                             : QStringLiteral("raised a non-string error"));
    lua_pop(state, 1);
    return false;
}

bool LuaPlugin::bookOpened(const Book &book, QString *error)
{
    if (!state) {
        *error = failure(QStringLiteral("is not loaded"));
        return false;
    }
    if (!pushHook("book_opened"))
        return true;
    pushBook(state, book);
    return call(1, 0, error);
}

bool LuaPlugin::bookClosed(QString *error)
{
    if (!state) {
        *error = failure(QStringLiteral("is not loaded"));
        return false;
    }
    if (!pushHook("book_closed"))
        return true;
    return call(0, 0, error);
}

bool LuaPlugin::beforeLookup(const Lookup &lookup, const Book &book, Card *card, QString *error)
{
    *card = Card();
    if (!state) {
        *error = failure(QStringLiteral("is not loaded"));
        return false;
    }
    if (!pushHook("before_lookup"))
        return true;
    lua_createtable(state, 0, 3);
    setField(state, "word", lookup.word);
    pushBook(state, book);
    lua_setfield(state, -2, "book");
    lua_createtable(state, 0, 4);
    lua_pushnumber(state, lookup.position.percent);
    lua_setfield(state, -2, "percent");
    lua_pushinteger(state, lookup.position.page);
    lua_setfield(state, -2, "page");
    lua_pushinteger(state, lookup.position.pages);
    lua_setfield(state, -2, "pages");
    setField(state, "chapter", lookup.position.chapter);
    lua_setfield(state, -2, "position");
    if (!call(1, 1, error))
        return false;

    const auto invalid = [&](const char *reason) {
        *error = failure(QStringLiteral("before_lookup ") + QLatin1String(reason));
        lua_pop(state, 1);
        return false;
    };
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
        return invalid("returned neither nil nor a table");
    Card result;
    for (const char *key : { "title", "html", "footer" }) {
        lua_pushstring(state, key);
        const int type = lua_rawget(state, -2);
        size_t bytes = 0;
        if (type == LUA_TSTRING)
            lua_tolstring(state, -1, &bytes);
        const bool html = qstrcmp(key, "html") == 0;
        const QString value =
                type == LUA_TSTRING && (!html || bytes <= htmlLimit) ? text(state, -1) : QString();
        lua_pop(state, 1);
        const bool footer = qstrcmp(key, "footer") == 0;
        if (type != LUA_TSTRING && !(footer && type == LUA_TNIL))
            return invalid(footer ? "returned a footer that is not a string"
                                  : "returned a card without a string title and html");
        // The sheet lays the card out on the main thread.
        if (html ? bytes > htmlLimit : value.size() > titleLimit)
            return invalid("returned a card past its size limits");
        if (!footer && value.isEmpty())
            return invalid("returned a card with an empty title or html");
        if (qstrcmp(key, "title") == 0)
            result.title = value;
        else if (html)
            result.html = value;
        else
            result.footer = value;
    }
    lua_pop(state, 1);
    *card = result;
    return true;
}
} // namespace kobalt::plugins
