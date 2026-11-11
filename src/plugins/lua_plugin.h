#pragma once
#include "plugin_types.h"

#include <QString>
#include <atomic>
#include <cstddef>

struct lua_State;
struct lua_Debug;

namespace kobalt::plugins {
struct Manifest
{
    QString id, name, version, directory;
    int api = 0;
};

// False on a missing or invalid plugin.json or main.lua; `error` says why.
bool readManifest(const QString &directory, Manifest *out, QString *error);

constexpr int defaultInstructionBudget = 5'000'000;
constexpr std::size_t defaultMemoryCap = 32 * 1024 * 1024;

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
    LuaPlugin(Manifest manifest, Context context, int instructionBudget = defaultInstructionBudget,
              std::size_t memoryCap = defaultMemoryCap);
    ~LuaPlugin();
    LuaPlugin(const LuaPlugin &) = delete;
    LuaPlugin &operator=(const LuaPlugin &) = delete;

    bool load(QString *error); // runs main.lua
    bool bookOpened(const Book &book, QString *error);
    bool bookClosed(QString *error);
    // false = failure (error/budget/memory/bad type); true with an empty card = no card
    bool beforeLookup(const Lookup &lookup, const Book &book, Card *card, QString *error);
    // True when main.lua defined before_lookup.
    bool answersLookups();
    const Manifest &manifest() const;

    // Allocations refused so far, for tests.
    std::size_t refusedAllocations() const { return memory.refused; }
    // While *abort is true, a running hook stops at its next budget check and
    // cannot make allocations larger than 64 KiB, so it fails.
    void setAbort(const std::atomic_bool *abort);

    struct Memory
    {
        std::size_t used = 0, cap = 0;
        bool enforce = false;
        const std::atomic_bool *abort = nullptr;
        std::size_t refused = 0;
    };

private:
    // Lets host pushes pass the cap, because a Lua memory error there would
    // unwind past C++ destructors. Callers check sizes first.
    class Unenforced
    {
    public:
        explicit Unenforced(lua_State *state);
        ~Unenforced();
        Unenforced(const Unenforced &) = delete;
        Unenforced &operator=(const Unenforced &) = delete;

    private:
        Memory &memory;
        bool enforced;
    };

    bool pushHook(const char *name);
    bool call(int arguments, int results, QString *error);
    QString failure(const QString &message) const;
    static void countHook(lua_State *state, lua_Debug *);
    static void *allocate(void *memory, void *block, std::size_t oldSize, std::size_t newSize);
    static std::size_t memoryLeft(lua_State *state);
    static int pushFile(lua_State *state, const QString &path);
    static int jsonDecode(lua_State *state);
    static int readFile(lua_State *state);
    static int readSidecar(lua_State *state);
    static int log(lua_State *state);
    static LuaPlugin *owner(lua_State *state);

    Manifest pluginManifest;
    Context context;
    QString root;
    int tickLimit;
    int ticks = 0;
    int logs = 0;
    const std::atomic_bool *abort = nullptr;
    Memory memory;
    lua_State *state = nullptr;
};
} // namespace kobalt::plugins
