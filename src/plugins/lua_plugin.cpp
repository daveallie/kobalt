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
#include <cmath>
#include <lua.hpp>

namespace kobalt::plugins {
namespace {
constexpr int hookInterval = 1000;
constexpr int logLimit = 1000;

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

bool readBytes(const QString &path, QByteArray *data)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *data = file.readAll();
    return true;
}

void pushBytesOrNil(lua_State *state, const QString &path)
{
    QByteArray data;
    if (readBytes(path, &data))
        lua_pushlstring(state, data.constData(), size_t(data.size()));
    else
        lua_pushnil(state);
}

bool pushJson(lua_State *state, const QJsonValue &value, int depth);

bool pushJsonObject(lua_State *state, const QJsonObject &object, int depth)
{
    lua_createtable(state, 0, int(object.size()));
    for (auto it = object.begin(); it != object.end(); ++it) {
        push(state, it.key());
        if (!pushJson(state, it.value(), depth + 1))
            return false;
        lua_rawset(state, -3);
    }
    return true;
}

bool pushJsonArray(lua_State *state, const QJsonArray &array, int depth)
{
    lua_createtable(state, int(array.size()), 0);
    for (int i = 0; i < array.size(); ++i) {
        if (!pushJson(state, array.at(i), depth + 1))
            return false;
        lua_rawseti(state, -2, i + 1);
    }
    return true;
}

bool pushJson(lua_State *state, const QJsonValue &value, int depth)
{
    if (!lua_checkstack(state, 4) || depth > 512)
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
        return pushJsonArray(state, value.toArray(), depth);
    case QJsonValue::Object:
        return pushJsonObject(state, value.toObject(), depth);
    default:
        lua_pushnil(state);
        return true;
    }
}

int jsonDecode(lua_State *state)
{
    size_t length = 0;
    const char *data = luaL_checklstring(state, 1, &length);
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
        decoded = document.isArray() ? pushJsonArray(state, document.array(), 0)
                                     : pushJsonObject(state, document.object(), 0);
    }
    if (!decoded) {
        lua_settop(state, 0);
        lua_pushnil(state);
        lua_pushliteral(state, "JSON is nested too deeply");
        return 2;
    }
    lua_settop(state, 1);
    return 1;
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
    for (const char *name : { "dofile", "loadfile", "require", "io", "os", "package", "debug" }) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
    lua_getglobal(state, "load");
    lua_pushcclosure(state, textLoad, 1);
    lua_setglobal(state, "load");
}
} // namespace

std::optional<Manifest> readManifest(const QString &directory, QString *error)
{
    const QDir folder(directory);
    const QString manifestPath = folder.filePath(QStringLiteral("plugin.json"));
    QByteArray data;
    if (!readBytes(manifestPath, &data)) {
        *error = QStringLiteral("cannot read ") + manifestPath;
        return std::nullopt;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error = manifestPath + QStringLiteral(" is not a JSON object: ") + parse.errorString();
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    Manifest manifest;
    manifest.id = object.value(QStringLiteral("id")).toString();
    if (manifest.id.isEmpty()) {
        *error = manifestPath + QStringLiteral(" has no id");
        return std::nullopt;
    }
    const QJsonValue api = object.value(QStringLiteral("api"));
    if (!api.isDouble() || api.toDouble() != 1) {
        *error = QStringLiteral("plugin ") + manifest.id
                + QStringLiteral(" needs an unsupported API version");
        return std::nullopt;
    }
    manifest.api = 1;
    manifest.name = object.value(QStringLiteral("name")).toString(manifest.id);
    manifest.version = object.value(QStringLiteral("version")).toString();
    manifest.directory = folder.absolutePath();
    if (!QFileInfo(folder.filePath(QStringLiteral("main.lua"))).isFile()) {
        *error = QStringLiteral("plugin ") + manifest.id + QStringLiteral(" has no main.lua");
        return std::nullopt;
    }
    return manifest;
}

LuaPlugin::LuaPlugin(Manifest manifest, Context context, int instructionBudget)
    : pluginManifest(std::move(manifest)),
      context(std::move(context)),
      root(QFileInfo(pluginManifest.directory).canonicalFilePath()),
      tickLimit(qMax(1, instructionBudget / hookInterval))
{
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

QString LuaPlugin::failure(const QString &message) const
{
    return QStringLiteral("plugin ") + pluginManifest.id + QStringLiteral(": ") + message;
}

LuaPlugin *LuaPlugin::owner(lua_State *state)
{
    return *static_cast<LuaPlugin **>(lua_getextraspace(state));
}

void LuaPlugin::countHook(lua_State *state, lua_Debug *)
{
    LuaPlugin *plugin = owner(state);
    if (++plugin->ticks <= plugin->tickLimit)
        return;
    // Fail at every instruction from now on, so that pcall cannot resume the loop.
    lua_sethook(state, countHook, LUA_MASKCOUNT, 1);
    luaL_error(state, "exceeded its instruction budget");
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
    if (path.isEmpty())
        lua_pushnil(state);
    else
        pushBytesOrNil(state, path);
    return 1;
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
    if (path.isEmpty() || !QFileInfo(path).isFile())
        lua_pushnil(state);
    else
        pushBytesOrNil(state, path);
    return 1;
}

int LuaPlugin::log(lua_State *state)
{
    luaL_checkstring(state, 1);
    const LuaPlugin *plugin = owner(state);
    const QString message = QStringLiteral("plugin:") + plugin->pluginManifest.id + QLatin1Char(' ')
            + text(state, 1).left(logLimit);
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
    if (!readBytes(QDir(pluginManifest.directory).filePath(QStringLiteral("main.lua")), &source)) {
        *error = failure(QStringLiteral("cannot read main.lua"));
        return false;
    }
    if (root.isEmpty()) {
        *error = failure(QStringLiteral("its folder is missing"));
        return false;
    }
    state = luaL_newstate();
    if (!state) {
        *error = failure(QStringLiteral("cannot create a Lua state"));
        return false;
    }
    *static_cast<LuaPlugin **>(lua_getextraspace(state)) = this;
    openLibraries(state);

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

bool LuaPlugin::call(int arguments, int results, QString *error)
{
    ticks = 0;
    lua_sethook(state, countHook, LUA_MASKCOUNT, hookInterval);
    const int status = lua_pcall(state, arguments, results, 0);
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

bool LuaPlugin::beforeLookup(const Lookup &lookup, const Book &book, std::optional<Card> *card,
                             QString *error)
{
    card->reset();
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
        const QString value = type == LUA_TSTRING ? text(state, -1) : QString();
        lua_pop(state, 1);
        const bool footer = qstrcmp(key, "footer") == 0;
        if (type != LUA_TSTRING && !(footer && type == LUA_TNIL))
            return invalid(footer ? "returned a footer that is not a string"
                                  : "returned a card without a string title and html");
        if (!footer && value.isEmpty())
            return invalid("returned a card with an empty title or html");
        if (qstrcmp(key, "title") == 0)
            result.title = value;
        else if (qstrcmp(key, "html") == 0)
            result.html = value;
        else
            result.footer = value;
    }
    lua_pop(state, 1);
    *card = result;
    return true;
}
} // namespace kobalt::plugins
