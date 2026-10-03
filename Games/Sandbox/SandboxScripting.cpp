#include "SandboxScripting.h"

#include <cmath>
#include <string>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

/*
 * Sandbox's script surface: a component of its own and a script library,
 * registered from this game module exactly as the engine registers its own.
 *
 * Every macro here registers when SandboxGame.dll loads and unregisters when it
 * unloads, so the editor can load the module to read the declarations
 * (Build > Refresh Script Nodes) and Play can load it again, any number of
 * times. Nothing here includes lua.h: the functions are plain C++, bound with
 * Native, and GameCore converts their arguments and results.
 *
 * In Lua (Scripts/Main.lua):
 *
 *     local pickup = Sandbox.SpawnPickup(220, 360, 0, 1)
 *     local value, id = pickup:GetComponent("Pickup"):GetStats()
 *     Events.On(Sandbox.ScoreChanged, function(score) ... end)
 *
 * In a script graph: Components/Pickup and Sandbox in the node palette.
 */

// === A game-defined value type ==============================================

/* Value then ID, in Lua and in a graph's Make / Break PickupStats. */
GAMECORE_DEFINE_SCRIPT_STRUCT(PickupStats, "PickupStats")
{
    Struct.Field("Value", &PickupStats::Value);
    Struct.Field("ID",    &PickupStats::ID);
}

// === A game-defined component ===============================================

GAMECORE_DEFINE_COMPONENT(SandboxPickupComponent, "Pickup")

// === A native prefab ========================================================

namespace
{
    /* A pickup: its data, the unit quad (sized by where it is placed), bounds
     * for picking, and a collider only the player's layer sets off --
     * collected by Scripts/Pickup.script.nmgraph's On Overlap Begin. */
    void BuildPickup(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Pickup");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<SandboxPickupComponent>();
        Root.Add<RenderCore::RenderComponent>();

        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));
        Root.Add(Bounds);

        GameCore::CollisionComponent Collision;
        Collision.Layer = SandboxLayers::Pickup;
        Collision.Mask  = SandboxLayers::Player;
        Root.Add(Collision);

        InBuilder.Script("Scripts/Pickup.script.nmgraph");
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB(Pickup, "Sandbox.Pickup", &BuildPickup)

namespace
{
    /* The bob, shared by Sandbox.Bob and SandboxPickupComponent::BobbedPosition. */
    double BobOffset(const double InSeconds, const double InHeight, const double InPhase)
    {
        return std::sin(InSeconds * 3.0 + InPhase) * InHeight;
    }
}

int32_t SandboxPickupComponent::Collect()
{
    const int32_t Worth = bCollected ? 0 : Stats.Value;
    bCollected = 1;
    return Worth;
}

EngineCore::Vector3 SandboxPickupComponent::BobbedPosition(const double InSeconds) const
{
    /* The phase spreads pickups along x out of step, as Pickup.script.nmgraph does. */
    const double Offset = BobOffset(InSeconds, BobHeight, Home.GetX() * 0.01);
    return EngineCore::Vector3(Home.GetX(), Home.GetY() + Offset, Home.GetZ());
}

/* Without a serializer the save/load demo would drop Pickup components and
 * leave their scripts erroring on a nil component. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(SandboxPickupComponent, 2)
{
    Ar.Field("Home",      C.Home);
    Ar.Field("Value",     C.Stats.Value);
    Ar.Field("BobHeight", C.BobHeight);
    Ar.Flag("Collected",  C.bCollected);
    if(Ar.GetVersion() >= 2) { Ar.Field("ID", C.Stats.ID); }   // 2: PickupStats
}

/* Lua: GetHome/SetHome, GetStats/SetStats (Value, ID), GetBobHeight/SetBobHeight,
 * GetCollected (read-only), Collect(), BobbedPosition(t). Graph:
 * Components/Pickup. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(SandboxPickupComponent)
{
    Bind.Field("Home",      &SandboxPickupComponent::Home);
    Bind.Field("Stats",     &SandboxPickupComponent::Stats);   // a script struct field
    Bind.Field("BobHeight", &SandboxPickupComponent::BobHeight);
    Bind.Flag("Collected",  &SandboxPickupComponent::bCollected, /*bInReadOnly=*/true);

    /* Member functions, exposed as they are: the glue and the pin types come
     * from their C++ signatures; the string only names the pins. */
    Bind.Native<&SandboxPickupComponent::Collect>("Collect", "-> Value");
    Bind.NativePure<&SandboxPickupComponent::BobbedPosition>("BobbedPosition", "Seconds -> Position");   // a query
}

// === The "Sandbox" script library ===========================================

namespace
{
    /* Variables a library exposes are C++ objects that outlive it: file-scope
     * statics of this module. */
    int32_t GScore        = 0;
    double  GElapsed      = 0.0;
    int32_t GNextPickupID = 0;

    /* Plain C++ functions. Lib.Native reads their arguments from the script,
     * calls them and returns the result; a World& parameter is filled in. */

    /* An instance of Sandbox.Pickup (below) at the position -- a 16 x 16
     * quad, by its placement's scale -- with this pickup's own value, ID
     * and home. Returns its root. */
    GameCore::EntityID SpawnPickup(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, const int32_t InValue)
    {
        GameCore::TransformPose At;
        At.Position = InPosition;
        At.Scale    = EngineCore::Vector3(16.0, 16.0, 1.0);
        const GameCore::Entity Pickup = InWorld.SpawnPrefab("native:Sandbox.Pickup", At).GetRoot();

        if(SandboxPickupComponent* Data = Pickup.GetComponent<SandboxPickupComponent>())
        {
            Data->Home        = InPosition;
            Data->Stats.Value = InValue;
            Data->Stats.ID    = ++GNextPickupID;
        }
        return Pickup.GetID();
    }

    /* Announces the new score from C++, as any system would: ScoreChanged on
     * the World's event bus. */
    int32_t AddScore(GameCore::World& InWorld, const int32_t InAmount)
    {
        GScore += InAmount;

        GameCore::EventArgs Args;
        Args.Args[0] = GameCore::EventArg::Make(double(GScore));
        Args.Count   = 1;
        InWorld.GetEvents().Emit("ScoreChanged", Args);
        return GScore;
    }

    const char* RankForScore(const int32_t InScore)
    {
        return InScore >= 12 ? "Gold" : InScore >= 6 ? "Silver" : InScore > 0 ? "Bronze" : "Unranked";
    }

    /* A script struct as an ordinary C++ parameter. */
    std::string DescribeStats(const PickupStats& InStats)
    {
        return "pickup #" + std::to_string(InStats.ID) + ", worth " + std::to_string(InStats.Value);
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Sandbox)
{
    Lib.Variable("Score", &GScore)
       .Tooltip("The player's score; AddScore also announces it");
    Lib.ReadOnly("Elapsed", &GElapsed)
       .Tooltip("Seconds since the game started, advanced by the game mode");

    /* The names become the graph's pins; the types come from C++. */
    Lib.Native<&SpawnPickup>("SpawnPickup", "Position, Value -> Pickup")
       .Tooltip("Creates a pickup worth Value at Position");
    Lib.Native<&AddScore>("AddScore", "Amount -> Score")
       .Tooltip("Adds Amount to Score and emits ScoreChanged");

    /* Pure: same inputs, same answer, so a graph evaluates it where needed. */
    Lib.NativePure<&BobOffset>("Bob", "Time, Height, Phase -> Offset")
       .Tooltip("A sine bob: Height * sin(Time * 3 + Phase)");
    Lib.NativePure<&RankForScore>("RankForScore", "Score -> Rank")
       .Tooltip("Unranked, Bronze, Silver or Gold");
    Lib.NativePure<&DescribeStats>("DescribeStats", "Stats -> Text")
       .Tooltip("\"pickup #3, worth 2\"");

    Lib.Event("PickupCollected", "Pickup:Entity, Value:integer")
       .Tooltip("A pickup was collected, worth Value");
    Lib.Event("ScoreChanged", "Score:integer")
       .Tooltip("Score changed; emitted by AddScore");
}

namespace SandboxScripting
{
    void SetElapsedSeconds(const double InSeconds) { GElapsed = InSeconds; }

    int32_t GetScore() { return GScore; }

    void SetScore(const int32_t InScore) { GScore = InScore; }
}
