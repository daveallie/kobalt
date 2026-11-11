#pragma once
#include "plugin_types.h"

#include <QString>
#include <optional>

struct lua_State;
struct lua_Debug;

namespace kobalt::plugins {
struct Manifest
{
    QString id, name, version, directory;
    int api = 0;
};

// Null on a missing or invalid plugin.json or main.lua; `error` says why.
std::optional<Manifest> readManifest(const QString &directory, QString *error);

struct Context
{
    QString bookFile; // local path or empty
    bool sidecars = true;
};

// One plugin's Lua state with a restricted library. Not thread-safe: every
// call must come from the thread that owns the plugin, never the GUI thread.
class LuaPlugin
{
public:
    LuaPlugin(Manifest manifest, Context context, int instructionBudget = 5'000'000);
    ~LuaPlugin();
    LuaPlugin(const LuaPlugin &) = delete;
    LuaPlugin &operator=(const LuaPlugin &) = delete;

    bool load(QString *error); // runs main.lua
    bool bookOpened(const Book &book, QString *error);
    bool bookClosed(QString *error);
    // false = failure (error/budget/bad type); true with nullopt = no card
    bool beforeLookup(const Lookup &lookup, const Book &book, std::optional<Card> *card,
                      QString *error);
    const Manifest &manifest() const;

private:
    bool pushHook(const char *name);
    bool call(int arguments, int results, QString *error);
    QString failure(const QString &message) const;
    static void countHook(lua_State *state, lua_Debug *);
    static int readFile(lua_State *state);
    static int readSidecar(lua_State *state);
    static int log(lua_State *state);
    static LuaPlugin *owner(lua_State *state);

    Manifest pluginManifest;
    Context context;
    QString root;
    int tickLimit;
    int ticks = 0;
    lua_State *state = nullptr;
};
} // namespace kobalt::plugins
