#ifndef NIMBLEENGINE_GAMEMODULE_H
#define NIMBLEENGINE_GAMEMODULE_H

#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreModule.h"

namespace EngineCore
{
    class ModuleManager;
}

namespace RenderCore
{
    class Renderer;
}

namespace NimbleEngine
{
    class ILevel;

    /*
     * A GAME, AS A LIBRARY: levels, modes, states and systems live in a module
     * DLL so both the game's launcher and the editor's Play-in-Editor can load
     * it by name (GameModuleHost). As an IModule it gets the ABI check and:
     *
     *   OnStartup   RegisterLevel every level (saves and the start level are
     *               resolved by name)
     *   OnShutdown  unregister them; the registry points into this DLL
     *
     * The host unloads the level before the module.
     *
     * Implement with NIMBLE_IMPLEMENT_GAME_MODULE, not NIMBLE_IMPLEMENT_MODULE.
     */
    class IGameModule : public EngineCore::IModule
    {
    public:
        /* Opened when [Game] StartLevel names none. Owned by the module. */
        virtual ILevel* GetStartLevel() = 0;
    };

    /* Export added by NIMBLE_IMPLEMENT_GAME_MODULE: casts IModule* to
     * IGameModule* inside the game's image, avoiding cross-DLL RTTI. */
    constexpr const char* GAME_MODULE_SYMBOL_CAST = "NimbleGameModuleCast";
    using GameModuleCastFn = IGameModule* (*)(EngineCore::IModule*);

    /*
     * Loads a game module from beside the executable and finds its start level.
     * File watching is OFF: reloading under a live World would leave dangling
     * system and delegate pointers. Main thread only.
     */
    class NE_API GameModuleHost
    {
    public:
        GameModuleHost();
        ~GameModuleHost();   // Unload()s

        GameModuleHost(const GameModuleHost&)            = delete;
        GameModuleHost& operator=(const GameModuleHost&) = delete;

        /* [Game] Module, else InDefaultName. Unloads any previous module. False,
         * logged, if missing, ABI-mismatched or not a game module. */
        bool Load(const std::string& InDefaultName);

        /* Null-safe. Unload the level first -- see IGameModule. */
        void Unload();

        bool         IsLoaded() const { return Game != nullptr; }
        IGameModule* GetGame()  const { return Game; }

        /* The name that was loaded, e.g. "SandboxGame". Empty if none. */
        const std::string& GetModuleName() const { return ModuleName; }

        /* [Game] StartLevel if registered, else GetStartLevel(); may be null. */
        ILevel* ResolveStartLevel() const;

        /* The renderer a loaded module's shader cache is mounted into: Load
         * mounts Shaders/Cache/<Module>.<Platform>.nmsc once the module has
         * started, Unload unmounts it first. Set before Load; null mounts
         * nothing. Not owned. */
        void SetRenderer(RenderCore::Renderer* InRenderer) { Rendering = InRenderer; }

    private:
        EngineCore::ModuleManager* Modules   = nullptr;
        IGameModule*               Game      = nullptr;
        RenderCore::Renderer*      Rendering = nullptr;
        std::string                ModuleName;
    };
}

/*
 * In ONE .cpp of the game module, at file scope:
 *
 *     NIMBLE_IMPLEMENT_GAME_MODULE(SandboxGameModule, "SandboxGame")
 *
 * The name must match the library file name ("<Name>.dll").
 */
#define NIMBLE_IMPLEMENT_GAME_MODULE(ClassName, ModuleNameLiteral)                          \
    NIMBLE_IMPLEMENT_MODULE(ClassName, ModuleNameLiteral)                                   \
    extern "C" NIMBLE_MODULE_API ::NimbleEngine::IGameModule*                               \
    NimbleGameModuleCast(::EngineCore::IModule* InModule)                                   \
    {                                                                                       \
        return static_cast<ClassName*>(InModule);                                           \
    }

#endif //NIMBLEENGINE_GAMEMODULE_H
