#ifndef NIMBLEENGINE_GAMEMODULE_H
#define NIMBLEENGINE_GAMEMODULE_H

#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreModule.h"

namespace EngineCore
{
    class ModuleManager;
}

namespace NimbleEngine
{
    class ILevel;

    /*
     * A GAME, AS A LIBRARY.
     *
     * A game's code -- its levels, modes, states and systems -- lives in a
     * module DLL rather than in the executable that runs it, so that more than
     * one program can run the same game: the game's own thin launcher, and the
     * editor's Play-in-Editor. Neither links the game; each loads it by name
     * through GameModuleHost.
     *
     * It is an EngineCore::IModule, so it gets the module manager's ABI check
     * (a Debug game against a Release engine is refused at load, not
     * discovered in a crash) and its lifecycle:
     *
     *   OnStartup   register every level with RegisterLevel -- a save names
     *               its level by string, and the host resolves the start
     *               level the same way
     *   OnShutdown  unregister them; the registry holds bare pointers into
     *               this DLL, which is about to be unmapped
     *
     * The host unloads the level BEFORE it unloads the module, so OnShutdown
     * never runs with a level of its own still live.
     *
     * Implement it with NIMBLE_IMPLEMENT_GAME_MODULE, not NIMBLE_IMPLEMENT_MODULE:
     * the extra export is how a host tells a game module from any other.
     */
    class IGameModule : public EngineCore::IModule
    {
    public:
        /* The level a host opens when [Game] StartLevel names none. Registered
         * by OnStartup like the rest, and owned by the module. */
        virtual ILevel* GetStartLevel() = 0;
    };

    /* The export NIMBLE_IMPLEMENT_GAME_MODULE adds: the module's IModule* as
     * the IGameModule it is. A function in the game's own image rather than a
     * cast in the host's, so no RTTI has to agree across the DLL boundary. */
    constexpr const char* GAME_MODULE_SYMBOL_CAST = "NimbleGameModuleCast";
    using GameModuleCastFn = IGameModule* (*)(EngineCore::IModule*);

    /*
     * Loads a game module and finds its start level: what a host does between
     * reading config and opening the first level.
     *
     * The module is looked for beside the executable (<Name>.dll on Windows),
     * where the build puts it next to the engine's own DLLs. It is loaded with
     * file watching OFF: reloading a game's code under a live World would
     * leave the World holding system and delegate pointers into the unmapped
     * image. Hot reload of game code wants the level unloaded first, and that
     * is a host decision, not this class's.
     *
     * Main thread only, like ModuleManager.
     */
    class NE_API GameModuleHost
    {
    public:
        GameModuleHost();
        ~GameModuleHost();   // Unload()s

        GameModuleHost(const GameModuleHost&)            = delete;
        GameModuleHost& operator=(const GameModuleHost&) = delete;

        /* [Game] Module when set, else InDefaultName. False, with the reason
         * logged, if the file is missing, the ABI differs, or the module is not
         * a game module. Loading while one is loaded unloads it first. */
        bool Load(const std::string& InDefaultName);

        /* Null-safe. Unload the level first -- see IGameModule. */
        void Unload();

        bool         IsLoaded() const { return Game != nullptr; }
        IGameModule* GetGame()  const { return Game; }

        /* The name that was loaded, e.g. "SandboxGame". Empty if none. */
        const std::string& GetModuleName() const { return ModuleName; }

        /* The level named by [Game] StartLevel if that is set and registered,
         * else the module's GetStartLevel(). Null if neither gives one. */
        ILevel* ResolveStartLevel() const;

    private:
        EngineCore::ModuleManager* Modules = nullptr;
        IGameModule*               Game    = nullptr;
        std::string                ModuleName;
    };
}

/*
 * In ONE .cpp of the game module, at file scope:
 *
 *     NIMBLE_IMPLEMENT_GAME_MODULE(SandboxGameModule, "SandboxGame")
 *
 * The name must match the library's file name -- the host loads "<Name>.dll".
 */
#define NIMBLE_IMPLEMENT_GAME_MODULE(ClassName, ModuleNameLiteral)                          \
    NIMBLE_IMPLEMENT_MODULE(ClassName, ModuleNameLiteral)                                   \
    extern "C" NIMBLE_MODULE_API ::NimbleEngine::IGameModule*                               \
    NimbleGameModuleCast(::EngineCore::IModule* InModule)                                   \
    {                                                                                       \
        return static_cast<ClassName*>(InModule);                                           \
    }

#endif //NIMBLEENGINE_GAMEMODULE_H
