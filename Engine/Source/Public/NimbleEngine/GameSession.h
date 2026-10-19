#ifndef NIMBLEENGINE_GAMESESSION_H
#define NIMBLEENGINE_GAMESESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/SplitCamera.h"
#include "Public/GameCore/ComponentType.h"   // ComponentTypeID
#include "Public/GameCore/Delegate.h"        // DelegateHandle
#include "Public/GameCore/EntityID.h"
#include "Public/Core/CoreArchive.h"      // EngineCore::Archive, for RestoreSaveInto below

namespace GameCore
{
    class World;
    class ScriptEngine;
}

namespace RenderCore
{
    class Renderer;
}

namespace UI
{
    class UISystem;
}

namespace JoltPhysics
{
    class PhysicsWorld;
}
namespace Audio
{
    class AudioWorld;
}

namespace NimbleEngine
{
    class GameConsole;
    class AvatarWorld;
    class GameSession;
    class GameState;
    class ILevel;

    /* Gives GameCore's CS_Mesh colliders their shapes from RenderCore's mesh
     * headers (CollisionMeshes.cpp). Done when this image loads and again by
     * every GameSession; idempotent. A host without a session calls it. */
    NE_API void InstallCollisionMeshProvider();

    /* Components whose module this build compiled out -- Rigid Body,
     * Physics Constraint and Vehicle without JoltPhysics, Net without NetCore -- load,
     * edit and save, but nothing acts on them. Warns (InContext names the
     * level or file) when InWorld has any; true if it did. Every level load
     * checks (and the first such component added later), as does the
     * editor's world open (InactiveModules.cpp). */
    NE_API bool WarnInactiveModuleComponents(const GameCore::World& InWorld, const char* InContext);

    /* InType is one of those components. */
    NE_API bool IsInactiveModuleComponent(GameCore::ComponentTypeID InType);

    /* [Modules] in the project's config (NetCore, JoltPhysics) asks for a
     * module this build was configured without, or the other way round --
     * edited (Project Settings) since the last configure. Warns, once per
     * call; true if it did. Application and the editor call it after loading
     * the config. */
    NE_API bool WarnModuleConfigMismatch();

    /* The session whose level owns InWorld, or nullptr: how the "Game" script
     * library and a native prefab's callbacks reach the rules. Set for the
     * whole of LoadLevel to UnloadLevel. Game thread only. */
    NE_API GameSession* FindGameSession(const GameCore::World& InWorld);

    /* A World with no game in it on purpose -- the UI Designer's Preview,
     * running a document's UI graph. The "Game" library there does nothing
     * and answers defaults, instead of raising "needs a running game".
     * nullptr clears it. Game thread only. */
    NE_API void SetScriptPreviewWorld(const GameCore::World* InWorld);

    /* Declared ahead of the friend declarations below to avoid MSVC C4273
     * (inconsistent linkage). Documented in SaveGame.h. */
    NE_API bool SaveGameToFile  (GameSession& InSession, const std::string& InPath);
    NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

    /*
     * Services the program running a session (Application, or the editor's
     * Play-in-Editor) offers the game. Keep it small: genuine host services
     * only, never a back door to the host's internals.
     */
    class IGameHost
    {
    public:
        virtual ~IGameHost() = default;

        /* Frame pacing target; 0 is unlimited. Takes effect from the next frame. */
        virtual void     SetFrameRateLimit(uint32_t InFramesPerSecond) = 0;
        virtual uint32_t GetFrameRateLimit() const = 0;

        /* This frame's input, latched after the event pump (InputRouter). A
         * host without input -- a test, a server -- keeps the default: nothing
         * pressed. */
        virtual const InputState& GetInput() const
        {
            static const InputState None;
            return None;
        }

        /* Mouse capture for mouse-look (InputRouter::SetMouseCaptured). A host
         * without a window ignores it. */
        virtual void SetMouseCaptured(bool /*bInCaptured*/) {}
        virtual bool IsMouseCaptured() const { return false; }

        /* Typed text into InputState::GetText (InputRouter::SetTextInputActive),
         * for the console. Off by default; a host without a window ignores it. */
        virtual void SetTextInputActive(bool /*bInActive*/) {}

        /* The system clipboard, for a UI text field's copy and paste. */
        virtual std::string GetClipboardText() const { return std::string(); }
        virtual void        SetClipboardText(const std::string& /*InUtf8*/) {}
    };

    /*
     * ONE RUNNING GAME: the active level, its World, mode and state, and the
     * load / swap / save / restore sequence. Each frame the host calls
     * ApplyPendingLevel() then Tick(); the host owns the window, event pump,
     * pacing, frame fence and present.
     *
     * The split lets the same game run in the game executable and in the
     * editor: game code only ever sees the session, never the host.
     *
     * NOT OWNED: the ScriptEngine and Renderer belong to the host and live for
     * the process. The session binds them to its World only while a level is
     * loaded. The World is the session's, one per loaded level.
     *
     * GAME THREAD ONLY: the thread that constructs the session. Every entry
     * that runs game code -- the load sequence, ApplyPendingLevel, Tick and
     * the framework calls below -- CORE_CHECKs it, so a mode, a level or a
     * script is never called from anywhere else and need not check for itself.
     */
    class NE_API GameSession
    {
    public:
        GameSession(IGameHost& InHost, GameCore::ScriptEngine& InScripts, RenderCore::Renderer& InRenderer);

        /* Unloads whatever is still loaded. */
        ~GameSession();

        GameSession(const GameSession&)            = delete;
        GameSession& operator=(const GameSession&) = delete;

        // === Driving it (the host) =============================================

        /*
         * THE LOAD SEQUENCE:
         *
         *   World created, scripts and renderer bound
         *   ILevel::OnLoad            the level populates the World (a listed
         *                             world: its file, then its mode is made)
         *   IGameMode::OnLoad
         *   the startup script        why the mode has two callbacks
         *   GameState::OnReset
         *   [only when restoring a save]
         *       ILevel::OnPrepareForLoadedGame (-> IGameMode's)
         *       the World, then the GameState, from the pending save
         *       IGameMode::OnPostLoadSave
         *   ApplyAutoPossess          placed avatars to their local players
         *   IGameMode::OnStart
         *
         * Restoring after OnReset leaves uncovered fields at defaults; before
         * OnStart, so the mode starts against restored state.
         *
         * Immediate: only where no World of this session is ticking (games use
         * RequestLevel). InLevel must outlive its time loaded. False if the
         * level or mode refused; the level stays loaded, so call UnloadLevel().
         */
        bool LoadLevel(ILevel& InLevel);

        /* Mode OnUnload, level OnUnload, unbind scripts and renderer, destroy
         * the World. Idempotent. */
        void UnloadLevel();

        /* Applies a pending RequestLevel() at the top of a frame. A level that
         * fails to load is unloaded and the session requests quit. */
        void ApplyPendingLevel();

        /* IGameMode::OnTick, then World::Tick. Overlaps the render thread
         * draining the previous frame. */
        void Tick(const FrameContext& InFrame);

        /* RequestQuit() was called or a requested level failed to load. The
         * host decides what that means (exit, or end play). */
        bool IsQuitRequested() const { return bQuitRequested; }

        // === What a game uses ====================================================

        /* Swap to InLevel (which must outlive its time loaded) at the next
         * ApplyPendingLevel(): a World cannot be destroyed inside its own Tick.
         * The outgoing level unloads first. The last request in a frame wins. */
        void RequestLevel(ILevel& InLevel);

        /* The game is finished. The host finishes the frame in flight. */
        void RequestQuit() { bQuitRequested = true; }

        /* nullptr before the first load and after the last unload. */
        ILevel*    GetLevel()     { return ActiveLevel; }
        /* GetLevel()->GetGameState(), or nullptr. */
        GameState* GetGameState();

        /* Valid from ILevel::OnLoad through OnUnload (CORE_CHECKed). A swap
         * replaces it; don't hold the reference. */
        GameCore::World& GetWorld();
        bool             HasWorld() const { return LevelWorld != nullptr; }

        /* Bumped by every LoadLevel. Use this, not the World pointer (which
         * can be reused), to detect a replaced World. */
        uint32_t GetLoadCount() const { return LoadCount; }

        /* The next ApplyPendingLevel will free the current World; drop any
         * bindings to it first. */
        bool HasPendingLevel() const { return PendingLevel != nullptr; }

        /* Lent by the host. Script classes survive a level swap; instances do
         * not. The Renderer is for RegisterProxyType and RenderStats. */
        GameCore::ScriptEngine& GetScriptEngine() { return Scripts; }
        RenderCore::Renderer&   GetRenderer()     { return Rendering; }

        IGameHost& GetHost() { return Host; }

        // === The game framework (GAMEFRAMEWORK.md) ==============================
        // Phases live in the GameState, players are PlayerStateComponents, and
        // these route every change through the mode's rules. All false / no-op
        // with no level, or a level without a GameState (phases) or mode.

        /*
         * Enter InPhase (at most GAME_PHASE_NAME_MAX - 1 characters). The mode
         * may refuse (IGameMode::CanEnterPhase); otherwise the state takes the
         * new phase and resets its timer, then IGameMode::OnPhaseChanged, then
         * the World event PhaseChanged(Phase, Previous) for scripts.
         *
         * Asked for while a change is being announced (from OnPhaseChanged or
         * a PhaseChanged listener), it is applied right after, so listeners
         * never see phases out of order; true means accepted for later. The
         * same phase as now is a no-op that returns true.
         */
        bool SetPhase(const char* InPhase);

        /* GetGameState()->GetPhase(), or "" without one. */
        const char* GetPhase();

        /* InPlayer's PlayerStateComponent Score / Lives plus InAmount (which
         * may be negative), then IGameMode::OnScoreChanged / OnLivesChanged and
         * the World event ScoreChanged / LivesChanged(Player, Value). False if
         * InPlayer has no PlayerState. */
        bool AddScore(GameCore::EntityID InPlayer, int32_t InAmount);
        bool AddLives(GameCore::EntityID InPlayer, int32_t InAmount);

        /* The entity whose PlayerState has PlayerIndex InIndex; unset if none. */
        GameCore::EntityID GetPlayer(uint8_t InIndex);
        uint32_t           GetPlayerCount();

        /* This frame's devices (Input.h), less what the UI took (GetUI).
         * Gameplay usually reads an InputComponent, resolved from this at the
         * start of TP_PrePhysics. */
        const InputState& GetInput() const { return FilteredInput; }

        // === Avatars (Avatar.h, AVATARS.md) =====================================

        /* The level's avatars: what feeds each AvatarHarness its player's
         * input. nullptr with no level. */
        AvatarWorld* GetAvatars() { return Avatars; }

        /*
         * The camera local player InLocalPlayer (0 first) views the game
         * through (and hears it from, with no Audio Listener): InCamera, an
         * entity with a CameraComponent or whose descendant has one -- a
         * cutscene, a death cam. Unset (the default) goes back to that
         * player's avatar's camera; player 1's then the level's first active
         * one. Cleared by a level change.
         */
        void               SetViewTarget(GameCore::EntityID InCamera, uint8_t InLocalPlayer = 0);
        GameCore::EntityID GetViewTarget(uint8_t InLocalPlayer = 0) const;

        /*
         * SPLITSCREEN (SPLITSCREEN.md): the window shows one view per local
         * player, player 1's first -- as many as the highest local player
         * with a view target (its own, or its avatar's camera), or InCount
         * (1 to 4) if set; 0 goes back to counting. Laid out by
         * RenderCore::MakeSplitscreenLayout: two players top and bottom, or
         * side by side with [Splitscreen] SideBySide; three, one across the
         * top. One view while a sequence holds the camera or plays a
         * cinematic, and always with [Splitscreen] Enabled off.
         */
        void     SetViewCount(uint32_t InCount);
        uint32_t GetViewCount() const { return ViewCount; }   // last frame's

        /*
         * SPLITSCREEN's dynamic split (SplitCamera.h, SPLITSCREEN.md "Dynamic
         * split"): with bEnabled, two to four local players' avatars are viewed
         * through cameras of the session's own, all turned one way -- shared
         * while the players fit, split by a turning line as two part, a camera
         * each in the fixed layout as three or four do. Their avatars' cameras
         * are not used. [Splitscreen] Dynamic and its Dynamic* keys until set.
         */
        void                SetSplitCamera(const SplitCameraSettings& InSettings);
        SplitCameraSettings GetSplitCamera() const;

        /* Hides and holds the cursor, for mouse-look; Mouse.DeltaX keeps
         * coming past the screen's edge. The host lets go when the run ends. */
        void SetMouseCaptured(bool bInCaptured) { Host.SetMouseCaptured(bInCaptured); }
        bool IsMouseCaptured() const            { return Host.IsMouseCaptured(); }

        // === The UI (Modules/UI/README.md) ======================================

        /*
         * This session's documents: opened from code (GetUI().Open), Lua
         * (UI.Open) or another document's actions, drawn over the default
         * scene. Each Tick gives it the input first and the game what it left
         * -- GetInput, the Input library and InputComponents all read that
         * (INPUT.md, "The UI") -- then updates it after the World. Its data
         * store has Player<N>.Score, Player<N>.Lives and Game.Phase, for
         * {Bind:...}. Open documents close with the session; a level change
         * closes all but the persistent ones.
         */
        UI::UISystem& GetUI() { return *Interface; }

        /*
         * Paused: the World does not tick and the mode's OnTick is not called;
         * the UI still runs, so a pause menu can resume. InputComponents keep
         * reading this frame's input, so a key the menu consumed does not
         * reach the game when it resumes. Also Game.SetPaused in Lua and the
         * native actions Game.Pause, Game.Resume and Game.TogglePause.
         */
        void SetPaused(bool bInPaused);
        bool IsPaused() const { return bPaused; }

        /* The in-game console (GameConsole.h, CONSOLE.md): the toggle key
         * opens it, and while open it takes the input before the UI does. */
        GameConsole& GetConsole() { return *Console; }

    private:
        /* Friends, not a public setter: only SaveGame.cpp may arm a restore. */
        friend NE_API bool SaveGameToFile(GameSession& InSession, const std::string& InPath);
        friend NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

        /* The view camera, to the renderer: SetViewTarget's, else the avatars'. */
        void PublishViewTarget();

        /* The before-scripts TP_PrePhysics system: InputComponents from GetInput(). */
        static void ResolveInput(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        /* LoadLevel's restore step. False on a corrupt save; the level stays loaded. */
        bool RestoreSaveInto(EngineCore::Archive& Ar, ILevel& InLevel, GameState* InState);

        /* EngineCore::appThreadID() at construction (an EngineCore::ThreadID). */
        uint64_t                GameThread = 0;

        IGameHost&              Host;
        GameCore::ScriptEngine& Scripts;
        RenderCore::Renderer&   Rendering;

        /* World::OnComponentAdded / Removed, for IGameMode::OnPlayerAdded / Removed. */
        static void OnComponentAdded(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);
        static void OnComponentRemoved(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

        /* SetPhase's change itself, with no queueing. */
        void ApplyPhase(const char* InPhase);

        /* CORE_CHECKs that the caller is on GameThread; InWhat names the entry. */
        void CheckGameThread(const char* InWhat) const;

        /* The level World's simulation (WITH_JOLTPHYSICS), from just after
         * the World is made to just before it goes; null without physics. */
        JoltPhysics::PhysicsWorld* Physics = nullptr;

        /* The level World's sound (WITH_AUDIO), likewise; null without the
         * module or an output device. */
        Audio::AudioWorld* LevelAudio = nullptr;

        /* This load warned about inactive-module components already, or has
         * not yet looked (WarnInactiveModuleComponents). */
        bool bInactiveChecked = false;
        bool bInactiveWarned  = false;

        /* LevelWorld lives from LoadLevel to UnloadLevel; same as ILevel::OwnedWorld. */
        ILevel*           ActiveLevel = nullptr;
        GameCore::World*  LevelWorld  = nullptr;
        AvatarWorld*      Avatars     = nullptr;
        GameCore::EntityID ViewTargets[INPUT_PLAYERS_MAX];   // SetViewTarget, by local player
        uint32_t          ViewCountWanted = 0;               // SetViewCount; 0 counts the players' views
        uint32_t          ViewCount       = 1;
        bool              bSequenceView   = false;           // a sequence set player 1's view target

        /* The dynamic split's state (SetSplitCamera): its cameras, made on first use in the
         * level's World, and its line's angle, kept between frames so it turns smoothly. */
        SplitCameraSettings SplitCamera;
        bool                bSplitCameraSet = false;
        GameCore::EntityID  SplitCameras[4];
        float               SplitAngle      = 0.0f;
        bool                bSplitAngleSet  = false;
        float               FrameDelta      = 0.0f;   // this Tick's, for the line's turn
        EngineCore::Size    FramePixels;              // and its window

        /* PublishViewTarget's dynamic split: true if it placed the views. */
        bool PublishSplitCamera();

        /* Set by RequestLevel, consumed by the next ApplyPendingLevel. */
        ILevel*           PendingLevel = nullptr;

        /* Armed with PendingLevel by LoadGameFromFile, cleared by the LoadLevel
         * that consumes it. (C4251 on this private member is harmless.) */
        std::vector<uint8_t> PendingSaveBytes;

        GameCore::DelegateHandle ComponentAddedHandle;
        GameCore::DelegateHandle ComponentRemovedHandle;

        /* SetPhase while a change is being announced: applied after it. Phase
         * names, in order. (C4251 on this private member is harmless.) */
        bool                     bChangingPhase = false;

        /* IGameMode::OnLoad succeeded and OnUnload has not begun: the player
         * hooks are live. */
        bool                     bModeLoaded = false;
        std::vector<std::string> QueuedPhases;

        bool bQuitRequested = false;
        bool bPaused        = false;
        bool bUITextInput   = false;   // a UI text field has the window's text input on
        bool bConsoleOpen   = false;   // last frame's: the console turns text input off as it closes

        /* The UI, and what it left of the host's input this frame. */
        UI::UISystem* Interface = nullptr;

        /* Made after Interface, destroyed before it: it draws a document. */
        GameConsole*  Console = nullptr;
        InputState    FilteredInput;
        std::string   PublishedPhase;

        /* The host's input to the UI, and what it took out of FilteredInput. */
        void RouteInputThroughUI();
        /* Player<N>.Score, Player<N>.Lives and Game.Phase into the UI's data store. */
        void PublishUIValues();

        /* "stat physics" ([Stats] Physics): the level's simulation into the
         * renderer's stats panel, or the section removed when it is off. */
        void PublishPhysicsStat();
        bool bPhysicsStatShown = false;

        /* "stat audio": the level's voices and the device (GStatsAudio). */
        void PublishAudioStat();
        bool bAudioStatShown = false;

        /* Cutscenes (Modules/Sequencer): the level's SequenceWorld's host.
         * The screen it asks for -- a fade, and in cinematic mode letterbox
         * bars and the players' input held back (Back skips) -- is a UI
         * document made in code, open while either is wanted. */
        static void               SequenceSetViewTarget(GameCore::EntityID InCamera, void* InUserData);
        static GameCore::EntityID SequenceGetViewTarget(void* InUserData);
        static void               SequenceSetFade(float InAlpha, void* InUserData);
        static void               SequenceSetCinematic(bool bInOn, void* InUserData);
        static bool               SequenceIsReplica(GameCore::EntityID InPlayer, void* InUserData);
        static void               SequenceSetBusGain(const char* InBus, float InGain, void* InUserData);
        static void               SequenceSetSubtitle(const char* InSpeaker, const char* InText, void* InUserData);
        void UpdateCinematicOverlay();
        bool  bCinematic    = false;
        float CinematicFade = -1.0f;
        bool  bSubtitle     = false;   // a line is up

        uint32_t LoadCount = 0;
    };
}

#endif //NIMBLEENGINE_GAMESESSION_H
