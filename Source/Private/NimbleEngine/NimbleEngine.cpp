#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreConfig.h"
#include "Public/GameWindow/GameWindowSystem.h"

NIMBLE_CONFIG_INT(GResolutionX, "Display", "ResolutionX", 1280, nullptr);
NIMBLE_CONFIG_INT(GResolutionY, "Display", "ResolutionY", 720, nullptr);
NIMBLE_CONFIG_STRING(GTitle,    "Application", "Title", "NimbleEngine", nullptr);

using namespace EngineCore;

namespace NimbleEngine
{
    bool Application::Initialize()
    {
        if(!GameWindowSystem::Initialize())
        {
            CORE_LOG_ERROR("Failed to Initialize Window")
            return false;
        }

        ConfigLoadStats Stats;
        if(!LoadConfigFile("Config/DefaultEngine.ini", &Stats))
        {
            CORE_LOG_WARN("No Config/DefaultEngine.ini Found")
        }

        WindowDesc Desc;
        Desc.Width = GResolutionX.GetInt();
        Desc.Height = GResolutionY.GetInt();
        Desc.Title = GTitle.GetString();
        Desc.Flags = WF_Resizable | WF_HighDPI;

        Window = GameWindowSystem::CreateGameWindow(Desc);
        if(!Window)
        {
            CORE_LOG_ERROR("Failed to Create Window")
            return false;
        }

        return true;
    }

    void Application::Exec()
    {
        if(!Initialize())
        {
            CORE_LOG("Failed to initialize Application!")
            return;
        }

        while(!bQuitRequested)
        {

        }
        Shutdown();
    }

    void Application::Shutdown()
    {
        if(Window)
        {
            
        }
    }
};