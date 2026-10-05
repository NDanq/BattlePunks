#include "Public/NimbleEngine/GameSession.h"
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

    // === Events (GameSession emits them on the World's EventBus) ===============

    Lib.Event("PhaseChanged", "Phase:string, Previous:string");
    Lib.Event("ScoreChanged", "Player:Entity, Score:integer");
    Lib.Event("LivesChanged", "Player:Entity, Lives:integer");
}
