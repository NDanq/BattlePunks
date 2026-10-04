#if WITH_EDITOR

#include "Private/Editor/PlaySession.h"

#include "Public/NimbleEngine/SaveGame.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameWindow/GameWindow.h"
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

bool PlaySession::Start(const std::string& InOpenWorld)
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

    /* The level of the world that is open, if one loads it; else [Game] StartLevel. */
    NimbleEngine::ILevel* Level = NimbleEngine::FindLevelByWorldFile(InOpenWorld.c_str());
    if(!Level)
    {
        Level = Game->ResolveStartLevel();
        if(Level && !InOpenWorld.empty())
        {
            CORE_LOG("[PIE] no level loads " + InOpenWorld + "; playing the start level")
        }
    }
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

void PlaySession::SetMouseCaptured(const bool bInCaptured)
{
    if(bInCaptured == IsMouseCaptured())
    {
        return;
    }
    bHostMouseCaptured = bInCaptured;
    Input.SetMouseCaptured(bInCaptured);
    if(!bInCaptured)
    {
        CORE_LOG("[PIE] the game released the mouse")
        return;
    }
    const GameWindow* Window = Input.GetWindow();
    CORE_LOG(std::string("[PIE] the game captured the mouse: ")
             + (!Window             ? "the viewport holds the cursor while it has the focus"
                : Window->HasFocus() ? "held in the viewport"
                                     : "held once the viewport has the input focus -- click it"))
}

void PlaySession::AttachInputWindow(GameWindow* InWindow)
{
    Input.Attach(InWindow);
    if(bHostMouseCaptured && InWindow)
    {
        Input.SetMouseCaptured(true);
        CORE_LOG(std::string("[PIE] the game's mouse capture moves to the viewport: ")
                 + (InWindow->HasFocus() ? "held in the viewport" : "held once the viewport has the input focus -- click it"))
    }
}

void PlaySession::Stop()
{
    const bool bWasRunning = Session != nullptr;

    /* The viewport's window outlives the run; it must not call into this one. */
    bHostMouseCaptured = false;
    Input.Detach();
    Input.GetPending() = NimbleEngine::InputState();
    Input.Latch(0);

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

    /* What arrived since the last frame becomes this frame's -- as the
     * launcher latches after its pump. Blocked, it is dropped unread. */
    if(bInputBlocked)
    {
        Input.GetPending() = NimbleEngine::InputState();
    }
    Input.Latch(FrameNumber);

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
