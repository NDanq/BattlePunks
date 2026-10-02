#if WITH_EDITOR

#include "Private/Editor/PlaySession.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/RenderCore/Renderer.h"

/* The game's own [Display] FrameRateLimit, read here only so GetFrameRateLimit
 * starts where the game's launcher would -- the same key Application reads. */
namespace
{
    int32_t ReadConfiguredFrameRateLimit()
    {
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Display", "FrameRateLimit");
        return Var && Var->GetType() == EngineCore::EConfigType::CT_Int ? Var->GetInt() : 0;
    }
}

PlaySession::PlaySession(RenderCore::Renderer& InRenderer)
    : Rendering(InRenderer)
{
}

PlaySession::~PlaySession()
{
    Stop();
}

bool PlaySession::Start()
{
    if(Session)
    {
        return true;
    }

    const int32_t Limit = ReadConfiguredFrameRateLimit();
    FrameRateLimit = Limit > 0 ? uint32_t(Limit) : 0u;
    FrameNumber    = 0;

    Game = EngineCore::Memory::Alloc<NimbleEngine::GameModuleHost>();
    Game->SetRenderer(&Rendering);
    if(!Game->Load(""))
    {
        CORE_LOG_ERROR("[PIE] no game module to play -- set [Game] Module in Config/DefaultEngine.ini")
        Stop();
        return false;
    }

    NimbleEngine::ILevel* Level = Game->ResolveStartLevel();
    if(!Level)
    {
        CORE_LOG_ERROR("[PIE] game module '" + Game->GetModuleName() + "' has no start level")
        Stop();
        return false;
    }

    Scripts = EngineCore::Memory::Alloc<GameCore::ScriptEngine>();
    if(!Scripts->Initialize())
    {
        CORE_LOG_ERROR("[PIE] the script engine failed to start")
        Stop();
        return false;
    }

    Session = EngineCore::Memory::Alloc<NimbleEngine::GameSession>(*this, *Scripts, Rendering);
    if(!Session->LoadLevel(*Level))
    {
        Stop();   // UnloadLevel undoes a partial load
        return false;
    }

    CORE_LOG("[PIE] playing " + Describe())
    return true;
}

void PlaySession::Stop()
{
    const bool bWasRunning = Session != nullptr;

    if(Session)
    {
        /* The render thread may still be drawing the level's last frame from
         * the proxies UnloadLevel is about to retire. */
        Rendering.WaitForPreviousFrame();

        Session->UnloadLevel();
        EngineCore::Memory::Dealloc(Session);
        Session = nullptr;
    }

    /* After the level: nothing may still hold a Lua reference. */
    if(Scripts)
    {
        Scripts->Shutdown();
        EngineCore::Memory::Dealloc(Scripts);
        Scripts = nullptr;
    }

    /* Last: the level's code is in it. */
    if(Game)
    {
        Game->Unload();
        EngineCore::Memory::Dealloc(Game);
        Game = nullptr;
    }

    if(bWasRunning)
    {
        CORE_LOG("[PIE] stopped")
    }
}

void PlaySession::Tick(const double InDeltaSeconds, const EngineCore::Size InViewportPixels)
{
    if(!Session)
    {
        return;
    }

    /* First, where no World is ticking -- as Application::RunFrame. */
    Session->ApplyPendingLevel();
    if(Session->IsQuitRequested() || !Session->HasWorld())
    {
        return;
    }

    NimbleEngine::FrameContext Frame;
    Frame.DeltaSeconds   = FrameNumber == 0 ? 0.0 : InDeltaSeconds;
    Frame.FrameNumber    = FrameNumber;
    Frame.ViewportPixels = InViewportPixels;
    Session->Tick(Frame);

    ++FrameNumber;
}

GameCore::World* PlaySession::GetWorld() const
{
    return Session && Session->HasWorld() ? &Session->GetWorld() : nullptr;
}

std::string PlaySession::Describe() const
{
    if(!Game || !Session || !Session->GetLevel())
    {
        return "(nothing)";
    }
    return Game->GetModuleName() + " / " + Session->GetLevel()->GetName();
}

#endif // WITH_EDITOR
