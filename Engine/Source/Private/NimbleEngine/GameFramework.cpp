#include <algorithm>
#include <string>

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/PlayerState.h"

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

/*
 * The "Game" script library: the game framework (GAMEFRAMEWORK.md) for Lua and
 * script graphs. Every member reaches the rules through the session that owns
 * the calling World (FindGameSession), so it means the same from a prefab's
 * script, the startup script or a graph:
 *
 *     if Game.IsPhase("Playing") then ... end
 *     Game.AddScore(Game.GetPlayer(0), 10)
 *     Events.On(Game.PhaseChanged, function(phase, previous) ... end)
 */

namespace
{
    const GameCore::World* GPreviewWorld = nullptr;

    /* Null, with a script error, outside a GameSession's World (a test's bare
     * World, the editor's scene) -- quietly null in a preview's World. */
    NimbleEngine::GameSession* SessionFor(GameCore::ScriptCall& InCall)
    {
        NimbleEngine::GameSession* Session = NimbleEngine::FindGameSession(InCall.GetWorld());
        if(!Session && &InCall.GetWorld() != GPreviewWorld)
        {
            InCall.Error("the Game library needs a running game (this World has no GameSession)");
        }
        return Session;
    }
}

namespace NimbleEngine
{
    void SetScriptPreviewWorld(const GameCore::World* InWorld)
    {
        GPreviewWorld = InWorld;
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Game)
{
    // === Pause =================================================================

    Lib.Function("SetPaused", [](GameCore::ScriptCall& Call)
    {
        if(NimbleEngine::GameSession* Session = SessionFor(Call)) { Session->SetPaused(Call.GetBool(1)); }
    }, "Paused:bool").Tooltip("Stops the World (and the mode's OnTick); the UI keeps running");

    Lib.Pure("IsPaused", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Session->IsPaused());
    }, "-> Paused:bool");

    // === Phase =================================================================

    Lib.Pure("GetPhase", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushString(Session ? Session->GetPhase() : "");
    }, "-> Phase:string").Tooltip("The game's current phase (GameState)");

    Lib.Pure("IsPhase", [](GameCore::ScriptCall& Call)
    {
        const char* Phase = Call.GetString(1);
        NimbleEngine::GameSession* Session = SessionFor(Call);
        const NimbleEngine::GameState* State = Session ? Session->GetGameState() : nullptr;
        Call.PushBool(State && State->IsPhase(Phase));
    }, "Phase:string -> Is:bool");

    Lib.Pure("GetPhaseTime", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        const NimbleEngine::GameState* State = Session ? Session->GetGameState() : nullptr;
        Call.PushNumber(State ? State->GetPhaseSeconds() : 0.0);
    }, "-> Seconds:number").Tooltip("Seconds since the current phase began");

    Lib.Pure("GetRound", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        const NimbleEngine::GameState* State = Session ? Session->GetGameState() : nullptr;
        Call.PushInteger(State ? State->GetRound() : 0);
    }, "-> Round:integer");

    Lib.Function("RequestPhase", [](GameCore::ScriptCall& Call)
    {
        const char* Phase = Call.GetString(1);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Session->SetPhase(Phase));
    }, "Phase:string -> Accepted:bool").Tooltip("Ask the game mode to enter Phase; false if its rules refuse");

    // === Level travel (LevelTravel.h) ============================================

    Lib.Function("OpenLevel", [](GameCore::ScriptCall& Call)
    {
        const std::string Name = Call.GetString(1);
        NimbleEngine::LevelTravel Travel;
        if(Call.HasArg(2)) { Travel.Entry = Call.GetString(2); }
        if(Call.HasArg(3)) { Travel.SetFromText(Call.GetString(3)); }
        if(Call.HasArg(4)) { Travel.Title = Call.GetString(4); }
        if(Call.HasArg(5)) { Travel.Tip = Call.GetString(5); }
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Session->OpenLevel(Name.c_str(), Travel));
    }, "Name:string, Entry:string, State:string, Title:string, Tip:string -> Accepted:bool")
       .Tooltip("Change level: a listed world, at a Player Start's Entry (\"\" usual), with travel state \"Alarm=1\"; "
                "Title/Tip: the loading card");

    Lib.Pure("IsInTransition", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Session->IsInTransition());
    }, "-> Transitioning:bool").Tooltip("A level transition is fading out, loading or fading in");

    Lib.Pure("GetEntryPoint", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushString(Session ? Session->GetTravel().Entry.c_str() : "");
    }, "-> Entry:string").Tooltip("The entry this level was travelled to at; \"\" for none");

    Lib.Pure("GetPreviousLevel", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushString(Session ? Session->GetTravel().Previous.c_str() : "");
    }, "-> Level:string").Tooltip("The level travelled from; \"\" for the first");

    Lib.Pure("GetTravelValue", [](GameCore::ScriptCall& Call)
    {
        const std::string Key     = Call.GetString(1);
        const std::string Default = Call.HasArg(2) ? Call.GetString(2) : "";
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushString(Session ? Session->GetTravelValue(Key.c_str(), Default.c_str()) : Default.c_str());
    }, "Key:string, Default:string -> Value:string").Tooltip("A travel value the level change carried in, or Default");

    Lib.Pure("GetTravelNumber", [](GameCore::ScriptCall& Call)
    {
        const std::string Key = Call.GetString(1);
        const double Default  = Call.HasArg(2) ? Call.GetNumber(2) : 0.0;
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushNumber(Session ? Session->GetTravel().GetNumber(Key.c_str(), Default) : Default);
    }, "Key:string, Default:number -> Value:number").Tooltip("A travel value as a number, or Default when absent or not one");

    // === Players ===============================================================

    Lib.Pure("GetPlayer", [](GameCore::ScriptCall& Call)
    {
        const int32_t Index = Call.GetInt(1);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushEntity(Session && Index >= 0 && Index < 256 ? Session->GetPlayer(uint8_t(Index)) : GameCore::EntityID());
    }, "Index:integer -> Player:Entity").Tooltip("The entity whose PlayerState has this PlayerIndex");

    Lib.Pure("GetPlayerCount", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushInteger(Session ? Session->GetPlayerCount() : 0);
    }, "-> Count:integer");

    Lib.Function("AddScore", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Player = Call.GetEntity(1);
        const int32_t            Amount = Call.GetInt(2);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        if(!Session) { return; }
        if(!Session->AddScore(Player, Amount))
        {
            Call.Error("Game.AddScore: the player has no PlayerState");
            return;
        }
        Call.PushInteger(Call.GetWorld().GetEntity(Player).GetComponent<NimbleEngine::PlayerStateComponent>()->Score);
    }, "Player:Entity, Amount:integer -> Score:integer").Tooltip("Add to a player's score, through the game mode's rules");

    Lib.Function("AddLives", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Player = Call.GetEntity(1);
        const int32_t            Amount = Call.GetInt(2);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        if(!Session) { return; }
        if(!Session->AddLives(Player, Amount))
        {
            Call.Error("Game.AddLives: the player has no PlayerState");
            return;
        }
        Call.PushInteger(Call.GetWorld().GetEntity(Player).GetComponent<NimbleEngine::PlayerStateComponent>()->Lives);
    }, "Player:Entity, Amount:integer -> Lives:integer").Tooltip("Add (or, negative, take) lives, through the game mode's rules");

    // === Avatars (Avatar.h) ====================================================

    Lib.Function("Possess", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Player = Call.GetEntity(1);
        const GameCore::EntityID Avatar = Call.GetEntity(2);
        if(Call.HasError()) { return; }
        if(NimbleEngine::HasAuthority())
        {
            Call.PushBool(NimbleEngine::Possess(Call.GetWorld(), Player, Avatar));
            return;
        }
        /* A client asks; the server's rules (Requestable, the mode's CanPossess) decide. */
        const GameCore::Entity Asking = Call.GetWorld().GetEntity(Player);
        Call.PushBool(Asking.HasComponent<NimbleEngine::PossessorComponent>()
                      && GAMECORE_RPC(NimbleEngine::PossessorComponent, ServerRequestPossess)(Asking, Avatar));
    }, "Player:Entity, Avatar:Entity -> Accepted:bool")
       .Tooltip("Player drives Avatar (an entity with an Avatar Harness). On a client, a request the server may refuse");

    Lib.Function("Unpossess", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Player = Call.GetEntity(1);
        if(Call.HasError()) { return; }
        if(NimbleEngine::HasAuthority())
        {
            Call.PushBool(NimbleEngine::Unpossess(Call.GetWorld(), Player));
            return;
        }
        const GameCore::Entity Asking = Call.GetWorld().GetEntity(Player);
        Call.PushBool(Asking.HasComponent<NimbleEngine::PossessorComponent>()
                      && GAMECORE_RPC(NimbleEngine::PossessorComponent, ServerRequestUnpossess)(Asking));
    }, "Player:Entity -> Accepted:bool").Tooltip("Player lets go of the avatar it drives");

    Lib.Pure("GetAvatar", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Player = Call.GetEntity(1);
        if(Call.HasError()) { return; }
        Call.PushEntity(NimbleEngine::GetAvatar(Call.GetWorld(), Player));
    }, "Player:Entity -> Avatar:Entity").Tooltip("What Player drives; nil if nothing");

    Lib.Pure("GetPossessor", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Avatar = Call.GetEntity(1);
        if(Call.HasError()) { return; }
        Call.PushEntity(NimbleEngine::GetPossessor(Call.GetWorld(), Avatar));
    }, "Avatar:Entity -> Player:Entity").Tooltip("Who drives Avatar; nil if nobody");

    Lib.Pure("GetLocalAvatar", [](GameCore::ScriptCall& Call)
    {
        const int32_t Local = Call.GetArgCount() >= 1 ? Call.GetInt(1) : 0;
        if(Call.HasError()) { return; }
        Call.PushEntity(Local >= 0 && Local < int32_t(NimbleEngine::INPUT_PLAYERS_MAX)
                        ? NimbleEngine::GetLocalAvatar(Call.GetWorld(), uint8_t(Local)) : GameCore::EntityID());
    }, "LocalPlayer:integer -> Avatar:Entity").Tooltip("The avatar this machine's local player (0 first) drives");

    Lib.Pure("GetLocalPlayer", [](GameCore::ScriptCall& Call)
    {
        const int32_t Local = Call.GetArgCount() >= 1 ? Call.GetInt(1) : 0;
        if(Call.HasError()) { return; }
        Call.PushEntity(Local >= 0 && Local < int32_t(NimbleEngine::INPUT_PLAYERS_MAX)
                        ? NimbleEngine::FindLocalPlayer(Call.GetWorld(), uint8_t(Local)) : GameCore::EntityID());
    }, "LocalPlayer:integer -> Player:Entity").Tooltip("This machine's local player (0 first): the entity with its Possessor");

    Lib.Function("SetViewTarget", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Camera = Call.GetEntity(1);
        const int32_t Local = Call.GetArgCount() >= 2 ? Call.GetInt(2) : 0;
        if(Call.HasError()) { return; }
        if(Local < 0 || Local >= int32_t(NimbleEngine::INPUT_PLAYERS_MAX)) { return; }
        if(NimbleEngine::GameSession* Session = SessionFor(Call)) { Session->SetViewTarget(Camera, uint8_t(Local)); }
    }, "Camera:Entity, LocalPlayer:integer?").Tooltip("View through Camera (or its child camera) -- local player LocalPlayer's view (0 first, the default) "
                                                       "in splitscreen; nil goes back to that player's avatar's");

    Lib.Function("JoinLocalPlayer", [](GameCore::ScriptCall& Call)
    {
        const int32_t Local = Call.GetInt(1);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Local > 0 && Local < int32_t(NimbleEngine::INPUT_PLAYERS_MAX) && Session->JoinLocalPlayer(uint8_t(Local)));
    }, "LocalPlayer:integer -> Joined:boolean").Tooltip("Split-screen: local player LocalPlayer (1-3) joins, as the mode allows");

    Lib.Function("LeaveLocalPlayer", [](GameCore::ScriptCall& Call)
    {
        const int32_t Local = Call.GetInt(1);
        if(Call.HasError()) { return; }
        NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushBool(Session && Local > 0 && Local < int32_t(NimbleEngine::INPUT_PLAYERS_MAX) && Session->LeaveLocalPlayer(uint8_t(Local)));
    }, "LocalPlayer:integer -> Left:boolean").Tooltip("Split-screen: local player LocalPlayer (1-3) leaves, their avatar and player gone");

    Lib.Pure("GetViewCount", [](GameCore::ScriptCall& Call)
    {
        const NimbleEngine::GameSession* Session = SessionFor(Call);
        Call.PushInteger(Session ? int32_t(Session->GetViewCount()) : 1);
    }, "-> Views:integer").Tooltip("How many views the window shows: one per local player in splitscreen, else 1");

    Lib.Function("SetViewCount", [](GameCore::ScriptCall& Call)
    {
        const int32_t Count = Call.GetInt(1);
        if(Call.HasError()) { return; }
        if(NimbleEngine::GameSession* Session = SessionFor(Call)) { Session->SetViewCount(uint32_t(std::clamp(Count, 0, 4))); }
    }, "Views:integer").Tooltip("Show this many views (1-4); 0 goes back to one per local player with a camera");

    // === Events (GameSession emits them on the World's EventBus) ===============

    Lib.Event("PhaseChanged", "Phase:string, Previous:string");
    Lib.Event("ScoreChanged", "Player:Entity, Score:integer");
    Lib.Event("LivesChanged", "Player:Entity, Lives:integer");
    Lib.Event("Possessed",    "Player:Entity, Avatar:Entity");
    Lib.Event("Unpossessed",  "Player:Entity, Avatar:Entity");
}
