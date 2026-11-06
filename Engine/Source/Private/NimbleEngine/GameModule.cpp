#include "Public/NimbleEngine/GameModule.h"

#include <filesystem>

#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreModuleManager.h"
#include "Public/GameWindow/GameWindowSystem.h"
#include "Public/RenderCore/Renderer.h"

/* Set in the game's Config/DefaultEngine.ini so launcher and editor agree. */
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

        /* Beside the executable: the working directory may differ. */
        const std::filesystem::path Path =
            std::filesystem::u8path(GameWindowSystem::GetExecutableDir()) / std::filesystem::u8path(Name + ModuleSuffix);
        /* std::string in C++17, std::u8string in C++20: copy either into a std::string. */
        const auto PathU8 = Path.u8string();
        const std::string PathText(PathU8.begin(), PathU8.end());

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

        /* Now that its modes are registered: a level per listed world. */
        RegisterWorldLevels();

        /* Its cooked materials: a file layer the renderer owns, not the DLL. */
        if(Rendering)
        {
            Rendering->MountShaderCache(Name.c_str());
        }
        return true;
    }

    void GameModuleHost::Unload()
    {
        if(ModuleName.empty() || !Modules)
        {
            return;
        }
        Game = nullptr;
        UnregisterWorldLevels();
        if(Rendering)
        {
            Rendering->UnmountShaderCache(ModuleName.c_str());
        }
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
                          + "' is not a listed world; using the first in Project Settings > Worlds")
        }
        const std::vector<WorldEntry> Worlds = ReadWorldList();
        return Worlds.empty() ? nullptr : FindLevelByName(Worlds.front().Name.c_str());
    }
}
