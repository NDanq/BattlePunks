#include "Public/NimbleEngine/GameModule.h"

#include <filesystem>

#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreModuleManager.h"
#include "Public/GameWindow/GameWindowSystem.h"

/* Which game a host runs, and where it starts. In the game's own
 * Config/DefaultEngine.ini, so its launcher and the editor agree. */
NIMBLE_CONFIG_STRING(GGameModule,     "Game", "Module",     "", nullptr);
NIMBLE_CONFIG_STRING(GGameStartLevel, "Game", "StartLevel", "", nullptr);

namespace NimbleEngine
{
    namespace
    {
#if defined(_WIN32)
        constexpr const char* ModuleSuffix = ".dll";
#elif defined(__APPLE__)
        constexpr const char* ModuleSuffix = ".dylib";
#else
        constexpr const char* ModuleSuffix = ".so";
#endif
    }

    GameModuleHost::GameModuleHost()
    {
        Modules = EngineCore::Memory::Alloc<EngineCore::ModuleManager>();
    }

    GameModuleHost::~GameModuleHost()
    {
        Unload();
        EngineCore::Memory::Dealloc(Modules);
        Modules = nullptr;
    }

    bool GameModuleHost::Load(const std::string& InDefaultName)
    {
        Unload();

        const std::string Name = GGameModule.GetString()[0] != '\0' ? std::string(GGameModule.GetString()) : InDefaultName;
        if(Name.empty())
        {
            CORE_LOG_ERROR("GameModuleHost: no game module named -- set [Game] Module")
            return false;
        }

        /* Beside the executable, not the working directory: an editor or a
         * test may run from elsewhere. */
        const std::filesystem::path Path =
            std::filesystem::u8path(GameWindowSystem::GetExecutableDir()) / std::filesystem::u8path(Name + ModuleSuffix);
        const std::string PathText = Path.u8string();

        if(!Modules->RegisterModule(Name, PathText, /*bInWatch=*/false))
        {
            return false;
        }
        if(!Modules->LoadModule(Name))
        {
            CORE_LOG_ERROR("GameModuleHost: could not load game module '" + Name + "' from " + PathText)
            Modules->UnregisterModule(Name);
            return false;
        }

        const auto Cast = reinterpret_cast<GameModuleCastFn>(Modules->GetModuleSymbol(Name, GAME_MODULE_SYMBOL_CAST));
        IGameModule* Loaded = Cast ? Cast(Modules->GetModule(Name)) : nullptr;
        if(!Loaded)
        {
            CORE_LOG_ERROR("GameModuleHost: '" + Name + "' is a module but not a game module "
                           "(no NIMBLE_IMPLEMENT_GAME_MODULE)")
            Modules->UnloadModule(Name);
            Modules->UnregisterModule(Name);
            return false;
        }

        Game       = Loaded;
        ModuleName = Name;
        CORE_LOG("GameModuleHost: loaded game module '" + Name + "' " + Modules->GetModuleVersionString(Name))
        return true;
    }

    void GameModuleHost::Unload()
    {
        if(ModuleName.empty() || !Modules)
        {
            return;
        }
        Game = nullptr;
        Modules->UnloadModule(ModuleName);     // OnShutdown unregisters the levels
        Modules->UnregisterModule(ModuleName);
        ModuleName.clear();
    }

    ILevel* GameModuleHost::ResolveStartLevel() const
    {
        if(!Game)
        {
            return nullptr;
        }

        const char* Named = GGameStartLevel.GetString();
        if(Named[0] != '\0')
        {
            if(ILevel* Found = FindLevelByName(Named))
            {
                return Found;
            }
            CORE_LOG_WARN(std::string("GameModuleHost: [Game] StartLevel '") + Named
                          + "' is not a registered level; using the module's start level")
        }
        return Game->GetStartLevel();
    }
}
