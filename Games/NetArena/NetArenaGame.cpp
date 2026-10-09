#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * NetArenaGame: networking's test game. Capsules on a flat plane, one per
 * player, each steered by its own player with WASD (or the left stick):
 * the first player's is red, the second's blue, then green and yellow.
 *
 * AUTHORED, NOT BUILT IN CODE. Everything placed is content the editor opens
 * and edits; this file only gives it behaviour:
 *
 *   Content/Levels/NetArena.nmworld   the plane, the sun and sky, the camera,
 *                                     four Player Starts (one kept for each
 *                                     player, by join order, each naming the
 *                                     Pawn prefab), and the Spinner -- a bar
 *                                     with a Net component, so the level
 *                                     itself has something networked in it.
 *   Content/Prefabs/Pawn.nmprefab     a player's capsule: Transform, Render
 *                                     (the capsule mesh, the PawnColor
 *                                     material), Net, NetArenaPawn and an
 *                                     Avatar Harness (Simulation
 *                                     NetArenaMove). The server spawns one per
 *                                     player.
 *
 * What a level entity with a Net component means: every process loads the
 * world, so every process has the Spinner; the server gives its copy a
 * NetId and the clients' copies are matched to it by GUID -- one entity in
 * the session, moved by the server (NetArena.Spin), seen turning by all.
 *
 * What a prefab with a Net component means: spawned on the server, it is
 * sent whole to every client (and to any that joins later); the server sets
 * who owns it (NetComponent::OwnerClient) and its colour after spawning.
 *
 * Server-authoritative, as NetCore is (Engine/Modules/NetCore/GUIDE.md), and
 * built on avatars (Engine/Source/AVATARS.md), so this file holds no
 * networking of its own:
 *
 *   - The server spawns the capsules (AvatarGameMode): one for each client
 *     that joins, and one for a listen server's or standalone game's own
 *     user, at the Player Start kept for it, and possesses each for its
 *     player.
 *   - Each capsule's Avatar Harness reads its player's keys on that
 *     player's process and sends them to the server. NetArenaMove, the
 *     harness's simulation, moves it from them -- on the server, the truth,
 *     replicated to everyone.
 *   - The server turns the Spinner (NetArena.Spin).
 *   - Two karts are placed in the level (Content/Prefabs/Kart.nmprefab): more
 *     avatars, Requestable, with a faster simulation (NetArenaKart) and a
 *     chase camera. Interact (E) gets a player in and out (NetArena.Interact,
 *     on the server, from the harness's input). Scripts/Kart.lua tints a kart
 *     in its driver's colour; Scripts/Signpost.script.nmgraph, on a placed
 *     signpost, shows the same avatar queries from a script graph.
 *
 * Smooth, and answering at once: a client shows other capsules and the
 * Spinner a moment behind, blended between the server's updates. Its own
 * capsule is predicted -- Pawn.nmprefab marks its Net component Predicted --
 * so the harness runs NetArenaMove there too the moment a key is down, and
 * replays the frames the server had not reached yet when its state comes
 * back. The 5-second report logs the worst correction: 0 when client and
 * server agree.
 *
 * Run it as:
 *
 *   NetArenaServer -port=7777                  + two of: NetArena -connect=127.0.0.1:7777
 *   NetArena -listen                           + one of: NetArena -connect=127.0.0.1:7777
 *   the editor: open Content/Levels/NetArena.nmworld; Edit > Editor Settings >
 *               Play > Network mode = Listen Server; Play (F5); then
 *               Play > New Client Window (Shift+F6)
 *
 *   NetArenaPawnComponent     a capsule: its colour.
 *   NetArenaMove              a capsule's move, from a frame of its input.
 *   NetArenaKart              a kart's: faster, keeping its heading.
 *   NetArenaSpinnerComponent  turns its entity, on the server.
 *   NetArenaGameMode          registers the system, colours the capsules
 *                             AvatarGameMode spawns, reports. [World_NetArena]
 *                             in Config/DefaultEngine.ini names it.
 */

namespace
{
    /* The arena: a square of this half-size around the origin. */
    constexpr double ArenaHalfSize  = 900.0;
    /* World units a second: on foot, and in a kart. */
    constexpr double PawnSpeed      = 450.0;
    constexpr double KartSpeed      = 900.0;
    /* How close a capsule must be to a kart to get in. */
    constexpr double KartReach      = 300.0;
    /* Seconds between the mode's log reports. */
    constexpr double ReportInterval = 5.0;

    struct PawnColor
    {
        const char* Name;
        float R, G, B;
    };
    constexpr PawnColor PawnColors[] = {
        { "red",    0.90f, 0.08f, 0.08f },
        { "blue",   0.08f, 0.25f, 0.95f },
        { "green",  0.10f, 0.80f, 0.20f },
        { "yellow", 0.95f, 0.85f, 0.10f },
    };
    constexpr int PawnColorCount = int(sizeof(PawnColors) / sizeof(PawnColors[0]));

    std::string Fixed0(const double InValue)
    {
        char Buffer[32];
        std::snprintf(Buffer, sizeof(Buffer), "%.0f", std::round(InValue) + 0.0);   // + 0.0 turns -0 into 0
        return Buffer;
    }
}

/* A bot at the keys, for unattended tests: each process steers its capsule
 * round a square of its own, a new side each second. -NetArena.AutoMove=1. */
NIMBLE_CONFIG_BOOL(GNetArenaAutoMove, "NetArena", "AutoMove", false, nullptr);
/* The bot also gets into the kart by its start after a second, and out
 * again five seconds later. -NetArena.AutoKart=1, with AutoMove. */
NIMBLE_CONFIG_BOOL(GNetArenaAutoKart, "NetArena", "AutoKart", false, nullptr);

// === Input ===================================================================

/* A player's keys, read by its capsule's Avatar Harness on its own process. */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Action("Interact",  "E, Gamepad.West");   // into the nearest kart, and out
}

// === Components ==============================================================

/* "NetArenaPawn": a player's capsule (Content/Prefabs/Pawn.nmprefab). Its
 * place is its Transform, which NetArenaMove moves on the server. */
struct NetArenaPawnComponent
{
    GAMECORE_COMPONENT_BODY()

    /* Which of PawnColors. The server sets it, and the Render colour, when
     * the capsule spawns; both reach clients with it. */
    uint8_t ColorIndex = 0;
};

GAMECORE_DEFINE_COMPONENT(NetArenaPawnComponent, "NetArenaPawn")

/* Saved in the prefab, and what a client gets when the capsule appears there. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaPawnComponent, 1)
{
    Ar.Field("ColorIndex", C.ColorIndex);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetArenaPawnComponent)
{
    Props.Choice("Color", &NetArenaPawnComponent::ColorIndex, "Red|Blue|Green|Yellow");
}
#endif

/* "NetArenaSpinner": turns its entity about Y. Only the server turns it; on
 * an entity with a Net component, clients see the server's Transform. */
struct NetArenaSpinnerComponent
{
    GAMECORE_COMPONENT_BODY()

    float DegreesPerSecond = 45.0f;
};

GAMECORE_DEFINE_COMPONENT(NetArenaSpinnerComponent, "NetArenaSpinner")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaSpinnerComponent, 1)
{
    Ar.Field("DegreesPerSecond", C.DegreesPerSecond);
}

/* Replicated too: a server that changes the speed changes it everywhere. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(NetArenaSpinnerComponent)
{
    Rep.Field("DegreesPerSecond", &NetArenaSpinnerComponent::DegreesPerSecond);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetArenaSpinnerComponent)
{
    Props.Field("Degrees per second", &NetArenaSpinnerComponent::DegreesPerSecond);
}
#endif

// === Movement ================================================================

namespace
{
    /* The longest frame a move may claim, so a client cannot cover more
     * ground than the time it has had. */
    constexpr float MaxMoveSeconds = 0.1f;

    /*
     * One frame of movement, InDirection (X right, Z forward, each -1..1) for
     * InSeconds, kept on the plane. THE ONE MOVE: the server's RPC, the
     * owner's prediction and its replay after a correction all call this, so
     * they agree to the bit -- prediction only works if they do.
     */
    void ApplyMove(GameCore::TransformComponent& InOut, const EngineCore::Vector3& InDirection, const float InSeconds,
                   const double InSpeed = PawnSpeed, const bool bInFace = true)
    {
        double X = std::clamp(double(InDirection.GetX()), -1.0, 1.0);
        double Z = std::clamp(double(InDirection.GetZ()), -1.0, 1.0);
        const double Length = std::sqrt(X * X + Z * Z);
        if(Length < 0.01) { return; }
        if(Length > 1.0) { X /= Length; Z /= Length; }   // diagonals no faster

        const double Step = InSpeed * double(std::clamp(InSeconds, 0.0f, MaxMoveSeconds));
        const double NewX = std::clamp(double(InOut.Position.GetX()) + X * Step, -ArenaHalfSize, ArenaHalfSize);
        const double NewZ = std::clamp(double(InOut.Position.GetZ()) + Z * Step, -ArenaHalfSize, ArenaHalfSize);
        InOut.Position = EngineCore::Vector3(NewX, InOut.Position.GetY(), NewZ);

        /* Face where it is going. */
        if(bInFace) { InOut.Rotation = EngineCore::Vector3(0.0, std::atan2(X, Z) * 57.29577951308232, 0.0); }
    }
}

/*
 * NetArenaMove: the Pawn's harness's simulation. One frame of a player's
 * input moves its capsule -- on the server for every frame its client sends,
 * on that client at once (prediction), and again there for the frames the
 * server had not reached when its state arrives. The engine clamps the
 * frame's seconds to MaxMoveSeconds; a client says where to go, never how far.
 */
NIMBLE_DEFINE_AVATAR_SIMULATION(NetArenaMove, MaxMoveSeconds)
{
    static const int Right   = NimbleEngine::FindInputAxis("Default", "MoveRight");
    static const int Forward = NimbleEngine::FindInputAxis("Default", "MoveForward");
    GameCore::TransformComponent* Transform = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
    if(!Transform || Right < 0 || Forward < 0) { return; }
    ApplyMove(*Transform, EngineCore::Vector3(double(InFrame.Axes[Right]), 0.0, double(InFrame.Axes[Forward])), InFrame.Seconds);
}

/*
 * NetArenaKart: a kart's harness's simulation (Content/Prefabs/Kart.nmprefab).
 * Twice as fast as on foot, and it keeps its heading, so the chase camera on
 * its child stays behind it while WASD still means up, left, down, right.
 */
NIMBLE_DEFINE_AVATAR_SIMULATION(NetArenaKart, MaxMoveSeconds)
{
    static const int Right   = NimbleEngine::FindInputAxis("Default", "MoveRight");
    static const int Forward = NimbleEngine::FindInputAxis("Default", "MoveForward");
    GameCore::TransformComponent* Transform = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
    if(!Transform || Right < 0 || Forward < 0) { return; }
    ApplyMove(*Transform, EngineCore::Vector3(double(InFrame.Axes[Right]), 0.0, double(InFrame.Axes[Forward])), InFrame.Seconds,
              KartSpeed, false);
}

// === Systems (server only) ===================================================

namespace
{
    /* PrePhysics: every spinner turns. */
    void SpinSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* /*InUserData*/)
    {
        if(!NimbleEngine::HasAuthority()) { return; }

        InWorld.Each<NetArenaSpinnerComponent>([&](GameCore::Entity InSpinner, NetArenaSpinnerComponent& InSpin)
        {
            if(GameCore::TransformComponent* Transform = InSpinner.GetComponent<GameCore::TransformComponent>())
            {
                const double Yaw = std::fmod(double(Transform->Rotation.GetY()) + double(InSpin.DegreesPerSecond) * InDeltaSeconds, 360.0);
                Transform->Rotation.SetY(float(Yaw));
            }
        });
    }
}

// === Mode ===================================================================

class NetArenaGameMode : public NimbleEngine::AvatarGameMode
{
public:
    /* The world (Content/Levels/NetArena.nmworld, every process the same
     * file) is loaded: give it behaviour. */
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        AvatarGameMode::OnLoad(InSession);
        InSession.GetWorld().RegisterSystem("NetArena.Spin", GameCore::TP_PrePhysics, &SpinSystem);
        InSession.GetWorld().RegisterSystem("NetArena.Interact", GameCore::TP_PrePhysics, &InteractSystem, this);
        Capsules.clear();
        if(NimbleEngine::AvatarWorld* Avatars = InSession.GetAvatars()) { Avatars->SetInputFilter(&AutoMove, this); }
        return true;
    }

    void OnStart() override
    {
        /* A server with a player of its own (a listen server), or no network
         * at all: this process's user gets the first capsule. A dedicated
         * server has no user; a client waits for the server's. */
        AvatarGameMode::OnStart();

        CORE_LOG(std::string("[NetArena] started as ")
                 + (NimbleEngine::IsDedicatedServer() ? "a dedicated server"
                    : NimbleEngine::IsServer()        ? "a listen server"
                    : NimbleEngine::IsClient()        ? "a client"
                                                      : "standalone")
                 + " -- WASD moves your capsule")
    }

    void OnClientDisconnected(const int32_t InClient, const GameCore::EntityID /*InPlayer*/) override
    {
        CORE_LOG("[NetArena] client " + std::to_string(InClient) + " left; its capsule goes with it")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        ElapsedSeconds += InFrame.DeltaSeconds;
        if(ElapsedSeconds - LastReport >= ReportInterval)
        {
            LastReport = ElapsedSeconds;
            Report(Session->GetWorld());
        }
    }

    void OnUnload() override
    {
        if(Session && Session->GetAvatars()) { Session->GetAvatars()->SetInputFilter(nullptr, nullptr); }
        Session = nullptr;
    }

protected:
    /* Each capsule in join order: red, blue, green, yellow. Set before the
     * end of the frame, when the server first sends it, so clients get it
     * this way. */
    void OnAvatarSpawned(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar, const uint32_t InOrdinal) override
    {
        GameCore::World& World = Session->GetWorld();
        const int Color = int(InOrdinal % uint32_t(PawnColorCount));
        const GameCore::Entity Pawn = World.GetEntity(InAvatar);
        if(NetArenaPawnComponent* State = Pawn.GetComponent<NetArenaPawnComponent>()) { State->ColorIndex = uint8_t(Color); }
        if(RenderCore::RenderComponent* Render = Pawn.GetComponent<RenderCore::RenderComponent>())
        {
            const PawnColor& Tint = PawnColors[Color];
            Render->SetMaterialParameter(0, "BaseColor", Tint.R, Tint.G, Tint.B, 0.0f);
        }
        CORE_LOG(std::string("[NetArena] ") + PawnColors[Color].Name + " capsule for " + World.GetEntity(InPlayer).GetName())
        Capsules[InPlayer.Pack()] = InAvatar;
    }

    /* Every process: the server as it happens, a client as it replicates. */
    void OnAvatarPossessed(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar) override
    {
        GameCore::World& World = Session->GetWorld();
        const NimbleEngine::AvatarHarnessComponent* Harness = World.GetEntity(InAvatar).GetComponent<NimbleEngine::AvatarHarnessComponent>();
        if(Harness && Harness->bRequestable)
        {
            CORE_LOG(std::string("[NetArena] ") + World.GetEntity(InPlayer).GetName() + " got into " + World.GetEntity(InAvatar).GetName())
        }
    }

    void OnAvatarUnpossessed(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar) override
    {
        GameCore::World& World = Session->GetWorld();
        if(!World.IsAlive(InAvatar) || !World.IsAlive(InPlayer)) { return; }
        const NimbleEngine::AvatarHarnessComponent* Harness = World.GetEntity(InAvatar).GetComponent<NimbleEngine::AvatarHarnessComponent>();
        if(Harness && Harness->bRequestable)
        {
            CORE_LOG(std::string("[NetArena] ") + World.GetEntity(InPlayer).GetName() + " got out of " + World.GetEntity(InAvatar).GetName())
        }
    }

private:
    /*
     * Server: Interact, from whatever a player drives. In a capsule, it gets
     * into the nearest free kart within KartReach (a Requestable harness);
     * in a kart, it gets out -- its capsule is put beside the kart, and
     * driven again. No RPC: the harness carried the press here.
     */
    static void InteractSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        if(!NimbleEngine::HasAuthority()) { return; }
        NetArenaGameMode& Self = *static_cast<NetArenaGameMode*>(InUserData);

        /* Collected first: Possess changes harnesses under Each. */
        std::vector<std::pair<GameCore::EntityID, GameCore::EntityID>> Asking;   // player, what it drives
        InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(InHarness.WasPressed("Interact") && InHarness.IsPossessed()) { Asking.push_back({ InHarness.GetPossessor(), InAvatar.GetID() }); }
        });

        for(const auto& [Player, Driven] : Asking)
        {
            const GameCore::EntityID Capsule = Self.Capsules[Player.Pack()];
            if(Driven != Capsule)
            {
                /* Out: the capsule waits where it was left; bring it to the kart. */
                if(!InWorld.IsAlive(Capsule)) { continue; }
                const GameCore::TransformComponent* KartAt = InWorld.GetEntity(Driven).GetComponent<GameCore::TransformComponent>();
                GameCore::TransformComponent*       At     = InWorld.GetEntity(Capsule).GetComponent<GameCore::TransformComponent>();
                if(KartAt && At)
                {
                    const double X = std::clamp(double(KartAt->Position.GetX()) + 150.0, -ArenaHalfSize, ArenaHalfSize);
                    At->Position = EngineCore::Vector3(X, At->Position.GetY(), KartAt->Position.GetZ());
                }
                NimbleEngine::Possess(InWorld, Player, Capsule);
                continue;
            }

            const GameCore::EntityID Kart = FindKartNear(InWorld, Driven);
            if(Kart.IsSet()) { NimbleEngine::Possess(InWorld, Player, Kart); }
        }
    }

    /* The nearest free kart within KartReach of InFrom, on the plane. */
    static GameCore::EntityID FindKartNear(GameCore::World& InWorld, const GameCore::EntityID InFrom)
    {
        const EngineCore::Vector3 From = InWorld.GetEntity(InFrom).GetWorldPose().Position;
        GameCore::EntityID Best;
        double BestDistance = KartReach;
        InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InKart, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(!InHarness.bRequestable || InHarness.IsPossessed()) { return; }
            const EngineCore::Vector3 At = InKart.GetWorldPose().Position;
            const double DX = double(At.GetX()) - double(From.GetX()), DZ = double(At.GetZ()) - double(From.GetZ());
            const double Distance = std::sqrt(DX * DX + DZ * DZ);
            if(Distance <= BestDistance) { BestDistance = Distance; Best = InKart.GetID(); }
        });
        return Best;
    }

    /* A bot at the keys (-NetArena.AutoMove=1): right, forward, left, back;
     * each process a side apart, so two bots go different ways. */
    static void AutoMove(GameCore::World& /*InWorld*/, GameCore::EntityID /*InAvatar*/, NimbleEngine::InputSnapshot& InOut, void* InUserData)
    {
        if(!GNetArenaAutoMove.GetBool()) { return; }
        static const int Right   = NimbleEngine::FindInputAxis("Default", "MoveRight");
        static const int Forward = NimbleEngine::FindInputAxis("Default", "MoveForward");
        if(Right < 0 || Forward < 0) { return; }
        static const double Sides[4][2] = { { 1.0, 0.0 }, { 0.0, 1.0 }, { -1.0, 0.0 }, { 0.0, -1.0 } };
        NetArenaGameMode& Self = *static_cast<NetArenaGameMode*>(InUserData);
        const int Side = (int(Self.ElapsedSeconds) + int(NimbleEngine::GetLocalClientId() + 1) * 2) & 3;
        InOut.Axes[Right]   = float(Sides[Side][0]);
        InOut.Axes[Forward] = float(Sides[Side][1]);

        /* AutoKart: stand still a second, by the kart near the start; press
         * Interact (in); drive; press it again five seconds later (out). */
        static const int Interact = NimbleEngine::FindInputAction("Default", "Interact");
        if(!GNetArenaAutoKart.GetBool() || Interact < 0) { return; }
        if(Self.ElapsedSeconds < 1.0) { InOut.Axes[Right] = InOut.Axes[Forward] = 0.0f; }
        for(double& At : Self.AutoKartPresses)
        {
            if(At > 0.0 && Self.ElapsedSeconds >= At)
            {
                InOut.Pressed |= 1u << Interact;
                At = 0.0;
                break;
            }
        }
    }

    /* Where each capsule is, and the spinner's angle, as this process sees
     * them: the test's evidence. */
    void Report(GameCore::World& InWorld)
    {
        std::string Line = "[NetArena]";
        InWorld.Each<NetArenaPawnComponent>([&](GameCore::Entity InPawn, NetArenaPawnComponent& InState)
        {
            const GameCore::TransformComponent* Transform = InPawn.GetComponent<GameCore::TransformComponent>();
            const GameCore::NetComponent*       Net       = InPawn.GetComponent<GameCore::NetComponent>();
            if(!Transform) { return; }
            Line += std::string(" ") + PawnColors[InState.ColorIndex % PawnColorCount].Name
                  + (Net && NimbleEngine::IsLocallyOwned(*Net) ? "(you)" : "")
                  + "=(" + Fixed0(Transform->Position.GetX()) + "," + Fixed0(Transform->Position.GetZ()) + ")";
        });
        InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InKart, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            const GameCore::TransformComponent* Transform = InKart.GetComponent<GameCore::TransformComponent>();
            if(!InHarness.bRequestable || !Transform) { return; }
            Line += std::string(" ") + InKart.GetName() + (InHarness.IsLocallyControlled() ? "(you)" : InHarness.IsPossessed() ? "(driven)" : "")
                  + "=(" + Fixed0(Transform->Position.GetX()) + "," + Fixed0(Transform->Position.GetZ()) + ")";
        });
        InWorld.Each<NetArenaSpinnerComponent>([&](GameCore::Entity InSpinner, NetArenaSpinnerComponent&)
        {
            if(const GameCore::TransformComponent* Transform = InSpinner.GetComponent<GameCore::TransformComponent>())
            {
                Line += " spinner=" + Fixed0(Transform->Rotation.GetY()) + "deg";
            }
        });

        /* Prediction's evidence: how many server states replayed, and the
         * furthest one moved the capsule from where it was shown. */
        const GameCore::EntityID Mine = NimbleEngine::GetLocalAvatar(InWorld, 0);
        NimbleEngine::AvatarWorld* Avatars = Session->GetAvatars();
        const NimbleEngine::AvatarPredictionStats Stats = Avatars && Mine.IsSet() ? Avatars->TakePredictionStats(Mine)
                                                                                    : NimbleEngine::AvatarPredictionStats();
        if(Stats.Corrections > 0)
        {
            Line += " predicted: " + std::to_string(Stats.Corrections) + " states, " + std::to_string(Stats.Pending)
                  + " pending, worst correction " + Fixed0(Stats.MaxCorrection) + "u";
        }
        CORE_LOG(Line)
    }

    double ElapsedSeconds = 0.0;
    double LastReport     = 0.0;
    double AutoKartPresses[2] = { 1.0, 6.0 };   // AutoKart's Interact presses, by ElapsedSeconds; 0 once done

    std::unordered_map<uint64_t, GameCore::EntityID> Capsules;   // each player's own, by packed id
};

// === Module =================================================================

/* Registers the mode the project's world list names. */
class NetArenaGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "NetArenaGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        NimbleEngine::RegisterGameMode<NetArenaGameMode>("NetArena");
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds this image's functions, and it is about to go. */
        NimbleEngine::UnregisterGameMode("NetArena");
    }
};

NIMBLE_IMPLEMENT_GAME_MODULE(NetArenaGameModule, "NetArenaGame")
