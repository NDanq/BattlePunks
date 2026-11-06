#include "Public/Sequencer/SequenceWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/Sequencer/SequencePlayerComponent.h"

/* SequenceWorld: the players that drive instances through time. README.md
 * has the rules; SequenceInstance.cpp what an instance does at a frame. */
namespace Sequencer
{
    namespace
    {
        std::unordered_map<const GameCore::World*, SequenceWorld*>& Systems()
        {
            static std::unordered_map<const GameCore::World*, SequenceWorld*> Map;
            return Map;
        }
    }

    SequenceWorld* SequenceWorld::Attach(GameCore::World& InWorld)
    {
        SequenceWorld*& System = Systems()[&InWorld];
        if(!System)
        {
            System = new SequenceWorld(InWorld);
            InWorld.RegisterSystem("Sequencer.Players", GameCore::TP_PostPhysics, &SequenceWorld::TickSystem, System);
            InWorld.RegisterSystem("Sequencer.Blend", GameCore::TP_PostUpdate, &SequenceWorld::BlendSystem, System);
        }
        ++System->Attachments;
        return System;
    }

    void SequenceWorld::Detach(GameCore::World& InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(&InWorld);
        if(Found == Map.end()) { return; }
        SequenceWorld* System = Found->second;
        if(--System->Attachments > 0) { return; }
        InWorld.UnregisterSystem(&SequenceWorld::TickSystem, System);
        InWorld.UnregisterSystem(&SequenceWorld::BlendSystem, System);
        Map.erase(Found);
        delete System;
    }

    SequenceWorld* SequenceWorld::Find(const GameCore::World* InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(InWorld);
        return Found != Map.end() ? Found->second : nullptr;
    }

    namespace
    {
        bool GSubtitlesEnabled = true;
    }

    SequenceWorld::SequenceWorld(GameCore::World& InWorld) : World(InWorld) {}

    void SequenceWorld::SetSubtitlesEnabled(const bool bInOn)
    {
        GSubtitlesEnabled = bInOn;
    }

    bool SequenceWorld::AreSubtitlesEnabled()
    {
        return GSubtitlesEnabled;
    }

    /* The world is going away: what the players made goes with it, nothing
     * restored, the view and screen left to the host (it is going too). The
     * mix is the audio device's, which outlives the level: put back. */
    SequenceWorld::~SequenceWorld()
    {
        if(!AppliedPostProcess.IsEmpty()) { PushPostProcess(World, SequencePostProcess()); }
        if(BlendCamera.IsSet() && World.IsAlive(BlendCamera)) { World.DestroyEntity(BlendCamera); }
        if(Host.SetBusGain)
        {
            for(const SequenceBusGain& Gain : AppliedBusGains) { Host.SetBusGain(Gain.Bus.c_str(), 1.0f, Host.UserData); }
        }
    }

    void SequenceWorld::SetHost(const SequenceHost& InHost)
    {
        Host = InHost;
    }

    void SequenceWorld::TickSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        (void)InWorld;
        static_cast<SequenceWorld*>(InUserData)->Tick(InDeltaSeconds);
    }

    void SequenceWorld::BlendSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        (void)InWorld;
        static_cast<SequenceWorld*>(InUserData)->TickBlend(InDeltaSeconds);
    }

    SequenceWorld::Player* SequenceWorld::FindPlayer(const GameCore::EntityID InPlayer)
    {
        const auto Found = Players.find(InPlayer.Pack());
        return Found != Players.end() ? &Found->second : nullptr;
    }

    const SequenceWorld::Player* SequenceWorld::FindPlayer(const GameCore::EntityID InPlayer) const
    {
        const auto Found = Players.find(InPlayer.Pack());
        return Found != Players.end() ? &Found->second : nullptr;
    }

    SequenceWorld::Player* SequenceWorld::GetPlayer(const GameCore::EntityID InPlayer)
    {
        if(!World.IsAlive(InPlayer) || !World.GetEntity(InPlayer).HasComponent<SequencePlayerComponent>())
        {
            return nullptr;
        }
        return &Players[InPlayer.Pack()];
    }

    SequenceInstance* SequenceWorld::FindInstance(const GameCore::EntityID InPlayer)
    {
        Player* P = FindPlayer(InPlayer);
        return P ? P->Instance.get() : nullptr;
    }

    bool SequenceWorld::IsReplica(const GameCore::EntityID InPlayer) const
    {
        return Host.IsReplica && Host.IsReplica(InPlayer, Host.UserData);
    }

    bool SequenceWorld::Prepare(const GameCore::EntityID InPlayer, Player& InOutPlayer)
    {
        const SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        const std::string Path(Component->Sequence, strnlen(Component->Sequence, SEQUENCE_PATH_MAX));

        /* The same file, as the cache has it now (the editor may have saved). */
        std::shared_ptr<const SequenceAsset> Asset = SequenceAssetCache::Find(Path);
        if(InOutPlayer.Instance && InOutPlayer.Instance->GetPath() == Path && Asset.get() == &InOutPlayer.Instance->GetAsset())
        {
            return true;
        }
        if(!Asset)
        {
            return false;
        }
        Release(InOutPlayer);
        InOutPlayer.Instance = std::make_unique<SequenceInstance>(World, std::move(Asset), Path);
        for(const auto& Override : InOutPlayer.Overrides)
        {
            InOutPlayer.Instance->SetBindingOverride(Override.first, Override.second);
        }
        return true;
    }

    void SequenceWorld::Release(Player& InOutPlayer)
    {
        if(!InOutPlayer.Instance) { return; }
        if(InOutPlayer.bRestoreState) { InOutPlayer.Instance->RestoreState(); }
        else                          { InOutPlayer.Instance->DiscardState(); }
        InOutPlayer.Instance->Release();
    }

    void SequenceWorld::Emit(const char* InEvent, const GameCore::EntityID InPlayer, const Player& InPlayerState)
    {
        GameCore::EventArgs Args;
        Args.Args[0] = GameCore::EventArg::Make(InPlayer);
        Args.Args[1] = GameCore::EventArg::Make(InPlayerState.Instance ? InPlayerState.Instance->GetPath().c_str() : "");
        Args.Count   = 2;
        World.GetEvents().Emit(InEvent, Args);
    }

    void SequenceWorld::Publish(const GameCore::EntityID InPlayer, const Player& InPlayerState, const bool bInJumped)
    {
        if(IsReplica(InPlayer) || !World.IsAlive(InPlayer)) { return; }
        SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        if(!Component) { return; }
        Component->NetState   = uint8_t(InPlayerState.State);
        Component->NetSeconds = InPlayerState.Instance ? float(InPlayerState.Instance->GetAsset().FramesToSeconds(InPlayerState.Frame)) : 0.0f;
        if(bInJumped) { ++Component->NetSerial; }
    }

    GameCore::EntityID SequenceWorld::PlaySequence(const char* InPath, const bool bInLoop)
    {
        if(!InPath || !SequenceAssetCache::Find(InPath))
        {
            return GameCore::EntityID();
        }
        const std::string Name = std::string("Sequence ") + InPath;
        GameCore::Entity Entity = World.CreateEntity(Name.c_str());

        SequencePlayerComponent Init;
        std::snprintf(Init.Sequence, sizeof(Init.Sequence), "%s", InPath);
        Init.bAutoPlay            = 0;
        Init.bLoop                = bInLoop ? 1 : 0;
        Init.bDestroyWhenFinished = 1;
        Entity.AddComponent(Init);

        if(!Play(Entity.GetID()))
        {
            World.DestroyEntity(Entity.GetID());
            return GameCore::EntityID();
        }
        return Entity.GetID();
    }

    bool SequenceWorld::Play(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        P->bAutoPlayed = true;
        if(P->State == ESequencePlayState::Paused && P->Instance)
        {
            return Resume(InPlayer);
        }
        /* Restarting: what the last run changed goes first, as it asked. */
        Release(*P);
        if(!Prepare(InPlayer, *P)) { return false; }

        P->bRestoreState = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>()->bRestoreState != 0;
        P->State         = ESequencePlayState::Playing;
        P->Frame         = 0.0;
        P->bIncludeFrame = true;
        P->bFresh        = true;
        P->StartOrder    = ++StartCounter;
        Publish(InPlayer, *P, true);
        Emit("SequenceStarted", InPlayer, *P);
        return true;
    }

    bool SequenceWorld::Pause(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(P->State == ESequencePlayState::Playing)
        {
            P->State = ESequencePlayState::Paused;
            if(P->Instance) { P->Instance->UpdatePlayback(P->Frame, false); }
            Publish(InPlayer, *P, false);
        }
        return true;
    }

    bool SequenceWorld::Resume(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(P->State == ESequencePlayState::Paused && P->Instance)
        {
            P->State = ESequencePlayState::Playing;
            Publish(InPlayer, *P, false);
        }
        return true;
    }

    bool SequenceWorld::Stop(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        const bool bWasStopped = P->State == ESequencePlayState::Stopped;
        Release(*P);
        P->State         = ESequencePlayState::Stopped;
        P->Frame         = 0.0;
        P->bIncludeFrame = false;
        Publish(InPlayer, *P, false);
        if(!bWasStopped)
        {
            Emit("SequenceStopped", InPlayer, *P);
        }
        ApplyToHost();
        return true;
    }

    bool SequenceWorld::Seek(const GameCore::EntityID InPlayer, const double InSeconds)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(!P->Instance)
        {
            if(!Prepare(InPlayer, *P)) { return false; }
            P->bRestoreState = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>()->bRestoreState != 0;
        }
        const SequenceAsset& Asset = P->Instance->GetAsset();
        P->Frame         = std::clamp(Asset.SecondsToFrames(InSeconds), 0.0, double(Asset.Length));
        P->bIncludeFrame = false;
        P->bAutoPlayed   = true;
        if(P->State == ESequencePlayState::Stopped || P->State == ESequencePlayState::Finished)
        {
            P->State      = ESequencePlayState::Paused;
            P->StartOrder = ++StartCounter;
        }
        P->Instance->NoteJump();
        P->Instance->Evaluate(P->Frame);
        P->Instance->UpdatePlayback(P->Frame, P->State == ESequencePlayState::Playing);
        Publish(InPlayer, *P, true);
        ApplyToHost();
        return true;
    }

    bool SequenceWorld::SeekToMarker(const GameCore::EntityID InPlayer, const char* InMarker)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P || !InMarker) { return false; }
        /* The asset as the cache has it now: the markers may be new. */
        if(!Prepare(InPlayer, *P)) { return false; }
        const SequenceAsset& Asset = P->Instance->GetAsset();
        const int32_t Marker = Asset.FindMarker(InMarker);
        return Marker >= 0 && Seek(InPlayer, Asset.FramesToSeconds(double(Asset.Markers[size_t(Marker)].Frame)));
    }

    bool SequenceWorld::Skip(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P || !P->Instance) { return false; }
        if(P->State != ESequencePlayState::Playing && P->State != ESequencePlayState::Paused) { return false; }

        SequenceInstance* Instance = P->Instance.get();
        const double End   = double(Instance->GetAsset().Length);
        const double From  = P->Frame;
        const bool   bIncl = P->bIncludeFrame;
        /* A blend out starts from what was seen, not the jump's last frame. */
        SequenceShot Shown;
        const GameCore::EntityID ShotBefore = Instance->GetCameraEntity();
        const bool bShown = ShotBefore.IsSet() && ShotBefore == AppliedCamera && ReadShownView(Shown);
        P->Frame         = End;
        P->bIncludeFrame = false;
        Instance->Evaluate(End);
        Instance->FireEvents(From, End, bIncl, true);

        /* A handler may have restarted or stopped it. */
        P = FindPlayer(InPlayer);
        if(P && P->Instance.get() == Instance && P->State != ESequencePlayState::Stopped)
        {
            Publish(InPlayer, *P, true);
            Finish(InPlayer, *P, bShown ? &Shown : nullptr);
        }
        ApplyToHost();
        return true;
    }

    size_t SequenceWorld::SkipCinematics()
    {
        std::vector<GameCore::EntityID> Skippable;
        for(const auto& Entry : Players)
        {
            const GameCore::EntityID Id = GameCore::EntityID::Unpack(Entry.first);
            if(Entry.second.State != ESequencePlayState::Playing || !World.IsAlive(Id)) { continue; }
            const SequencePlayerComponent* Component = World.GetEntity(Id).GetComponent<SequencePlayerComponent>();
            if(Component && Component->bCinematic && Component->bSkippable && !IsReplica(Id)) { Skippable.push_back(Id); }
        }
        size_t Count = 0;
        for(const GameCore::EntityID Id : Skippable) { Count += Skip(Id) ? 1 : 0; }
        return Count;
    }

    bool SequenceWorld::Bind(const GameCore::EntityID InPlayer, const char* InBindingId, const GameCore::EntityID InEntity)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P || !InBindingId || !InBindingId[0]) { return false; }
        if(InEntity.IsSet()) { P->Overrides[InBindingId] = InEntity; }
        else                 { P->Overrides.erase(InBindingId); }
        if(P->Instance)
        {
            return P->Instance->SetBindingOverride(InBindingId, InEntity);
        }
        return true;
    }

    ESequencePlayState SequenceWorld::GetState(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P ? P->State : ESequencePlayState::Stopped;
    }

    double SequenceWorld::GetTime(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P && P->Instance ? P->Instance->GetAsset().FramesToSeconds(P->Frame) : 0.0;
    }

    double SequenceWorld::GetDuration(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P && P->Instance ? P->Instance->GetAsset().GetDurationSeconds() : 0.0;
    }

    void SequenceWorld::Finish(const GameCore::EntityID InPlayer, Player& InOutPlayer, const SequenceShot* InSkippedFrom)
    {
        const SequencePlayerComponent* Component = World.IsAlive(InPlayer)
            ? World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>() : nullptr;

        /* Its shot goes with Release: what it showed is kept for the blend
         * out, which ApplyToHost starts once no other player has a shot. */
        const GameCore::EntityID Shot = InOutPlayer.Instance ? InOutPlayer.Instance->GetCameraEntity() : GameCore::EntityID();
        if(Component && Component->BlendOut > 0.0f && Shot.IsSet() && Shot == AppliedCamera)
        {
            SequenceShot View;
            if(InSkippedFrom) { View = *InSkippedFrom; }
            if(InSkippedFrom || ReadShownView(View))
            {
                PendingOut.bValid       = true;
                PendingOut.Camera       = Shot;
                PendingOut.View         = View;
                PendingOut.Seconds      = InSkippedFrom ? Component->BlendOut * 0.5f : Component->BlendOut;
                PendingOut.Ease         = Component->BlendEase;
                PendingOut.bHoldControl = Component->bCinematic && Component->ReleaseControl == SCR_AfterBlend;
            }
        }

        InOutPlayer.State = ESequencePlayState::Finished;
        Release(InOutPlayer);
        Publish(InPlayer, InOutPlayer, false);
        const bool bDestroy = Component && Component->bDestroyWhenFinished && !IsReplica(InPlayer);

        Emit("SequenceFinished", InPlayer, InOutPlayer);
        if(bDestroy && World.IsAlive(InPlayer))
        {
            World.DestroyEntity(InPlayer);
        }
    }

    void SequenceWorld::Advance(const GameCore::EntityID InPlayer, Player& InOutPlayer, const float InDeltaSeconds)
    {
        const SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        SequenceInstance*    Instance = InOutPlayer.Instance.get();
        const SequenceAsset& Asset    = Instance->GetAsset();
        const double         Length   = double(Asset.Length);
        const double         Rate     = std::max(0.0, double(Component->PlayRate));

        const double From  = InOutPlayer.Frame;
        const bool   bIncl = InOutPlayer.bIncludeFrame;
        double       To    = From + double(InDeltaSeconds) * double(Asset.FrameRate) * Rate;
        InOutPlayer.bIncludeFrame = false;

        bool bEnded = false;
        if(To >= Length)
        {
            if(Component->bLoop && Length > 0.0)
            {
                /* The rest of this pass, then the start of the next. */
                const double Wrapped = std::fmod(To, Length);
                InOutPlayer.Frame = Wrapped;
                Instance->NoteJump();
                Instance->Evaluate(Wrapped);
                Instance->UpdatePlayback(Wrapped, true);
                Instance->FireEvents(From, Length, bIncl);
                Player* P = FindPlayer(InPlayer);
                if(P && P->Instance.get() == Instance && P->State == ESequencePlayState::Playing)
                {
                    Instance->FireEvents(0.0, Wrapped, true);
                }
                return;
            }
            To     = Length;
            bEnded = true;
        }

        InOutPlayer.Frame = To;
        Instance->Evaluate(To);
        Instance->UpdatePlayback(To, !bEnded);
        Instance->FireEvents(From, To, bIncl);

        if(bEnded)
        {
            Player* P = FindPlayer(InPlayer);
            if(P && P->Instance.get() == Instance && P->State == ESequencePlayState::Playing)
            {
                Finish(InPlayer, *P);
            }
        }
    }

    void SequenceWorld::Follow(const GameCore::EntityID InPlayer, Player& InOutPlayer)
    {
        const SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        const auto Wanted  = ESequencePlayState(std::min<uint8_t>(Component->NetState, uint8_t(ESequencePlayState::Finished)));
        const bool bJumped = Component->NetSerial != InOutPlayer.SeenSerial;
        const bool bState  = Component->NetState != InOutPlayer.SeenState;
        if(!bJumped && !bState) { return; }
        InOutPlayer.SeenSerial = Component->NetSerial;
        InOutPlayer.SeenState  = Component->NetState;
        const double Seconds   = Component->NetSeconds;

        switch(Wanted)
        {
            case ESequencePlayState::Stopped:
                if(InOutPlayer.State != ESequencePlayState::Stopped) { Stop(InPlayer); }
                break;
            case ESequencePlayState::Finished:
                if(InOutPlayer.State == ESequencePlayState::Playing || InOutPlayer.State == ESequencePlayState::Paused) { Skip(InPlayer); }
                break;
            case ESequencePlayState::Playing:
            case ESequencePlayState::Paused:
                if(bJumped)
                {
                    /* Started over where the server's is: from the start's
                     * events if it is at the start, else seeked there. */
                    if(InOutPlayer.State != ESequencePlayState::Playing || Seconds <= 0.0) { Play(InPlayer); }
                    if(Seconds > 0.0) { Seek(InPlayer, Seconds); }
                }
                if(Wanted == ESequencePlayState::Paused) { Pause(InPlayer); }
                else                                     { Resume(InPlayer); }
                break;
        }
    }

    void SequenceWorld::ApplyToHost()
    {
        /* The shot of the player started last; the darkest fade; any cinematic. */
        GameCore::EntityID Camera;
        const Player*                  Shown          = nullptr;
        const SequencePlayerComponent* ShownComponent = nullptr;
        uint64_t Latest   = 0;
        float    Fade     = -1.0f;
        bool     bCinema  = false;
        for(const auto& Entry : Players)
        {
            const Player& P = Entry.second;
            if(!P.Instance || (P.State != ESequencePlayState::Playing && P.State != ESequencePlayState::Paused)) { continue; }
            const GameCore::EntityID Id = GameCore::EntityID::Unpack(Entry.first);
            const SequencePlayerComponent* Component = World.IsAlive(Id)
                ? World.GetEntity(Id).GetComponent<SequencePlayerComponent>() : nullptr;
            const GameCore::EntityID Shot = P.Instance->GetCameraEntity();
            if(Shot.IsSet() && P.StartOrder >= Latest)
            {
                Camera         = Shot;
                Latest         = P.StartOrder;
                Shown          = &P;
                ShownComponent = Component;
            }
            Fade = std::max(Fade, P.Instance->GetFade());
            bCinema = bCinema || (Component && Component->bCinematic && !HasReleasedControl(P, *Component));
        }

        if(Camera != AppliedCamera)
        {
            const GameCore::EntityID Previous = AppliedCamera;
            AppliedCamera = Camera;   // first: a blend in hands over to it
            if(Camera.IsSet())
            {
                if(!bViewTaken)
                {
                    ViewBefore = Host.GetViewTarget ? Host.GetViewTarget(Host.UserData) : GameCore::EntityID();
                    bViewTaken = true;
                }
                /* Into the first shot, from the game's view (or a blend out
                 * back to it), while the sequence is still in its BlendIn. */
                const float  In   = ShownComponent ? ShownComponent->BlendIn : 0.0f;
                const double Into = Shown->Instance->GetAsset().FramesToSeconds(Shown->Frame);
                bool bBlending = false;
                if(In > 0.0f && Into < double(In) && !Previous.IsSet())
                {
                    ViewBlend Next;
                    if(Blend.bActive) { bBlending = ReadShownView(Next.FromView); }
                    else if(ViewBefore.IsSet() && World.IsAlive(ViewBefore))
                    {
                        Next.FromEntity = ViewBefore;
                        bBlending = ReadCameraView(World, ViewBefore, Next.FromView);
                    }
                    if(bBlending)
                    {
                        Next.bActive  = true;
                        Next.ToEntity = Camera;
                        Next.Seconds  = In;
                        Next.Elapsed  = float(Into);
                        Next.Ease     = ShownComponent->BlendEase;
                        Blend = Next;
                        StartBlend();
                    }
                }
                if(!bBlending)
                {
                    if(Host.SetViewTarget) { Host.SetViewTarget(Camera, Host.UserData); }
                    EndBlend(false);
                }
            }
            else if(PendingOut.bValid && PendingOut.Camera == Previous && PendingOut.Seconds > 0.0f)
            {
                /* Out of the last shot, back to the view it took. */
                ViewBlend Next;
                Next.bActive      = true;
                Next.bOut         = true;
                Next.bHoldControl = PendingOut.bHoldControl;
                Next.FromView     = PendingOut.View;
                Next.Seconds      = PendingOut.Seconds;
                Next.Ease         = PendingOut.Ease;
                if(ViewBefore.IsSet() && World.IsAlive(ViewBefore)) { Next.ToEntity = ViewBefore; }
                else
                {
                    Next.ToEntity   = ProbeDefaultView();
                    Next.bToDefault = true;
                }
                Blend = Next;
                SequenceShot Probe;
                if(ReadCameraView(World, Blend.ToEntity, Probe)) { StartBlend(); }
                else                                              { EndBlend(true); }   // nothing to ease to: given back now
            }
            else
            {
                const GameCore::EntityID Back = ViewBefore.IsSet() && World.IsAlive(ViewBefore) ? ViewBefore : GameCore::EntityID();
                if(Host.SetViewTarget) { Host.SetViewTarget(Back, Host.UserData); }
                EndBlend(false);
                bViewTaken = false;
                ViewBefore = GameCore::EntityID();
            }
        }
        PendingOut = PendingBlendOut();

        /* A blend out keeps its player's hold until it has run (SCR_AfterBlend). */
        bCinema = bCinema || (Blend.bActive && Blend.bOut && Blend.bHoldControl);
        if(Fade != AppliedFade)
        {
            AppliedFade = Fade;
            if(Host.SetFade) { Host.SetFade(Fade, Host.UserData); }
        }
        /* Post process: earlier-started players first, so a later one's settings win. */
        std::vector<const Player*> Playing;
        for(const auto& Entry : Players)
        {
            const Player& P = Entry.second;
            if(P.Instance && (P.State == ESequencePlayState::Playing || P.State == ESequencePlayState::Paused)) { Playing.push_back(&P); }
        }
        std::sort(Playing.begin(), Playing.end(), [](const Player* InA, const Player* InB) { return InA->StartOrder < InB->StartOrder; });
        SequencePostProcess Post;
        for(const Player* P : Playing) { Post.Append(P->Instance->GetPostProcess()); }
        if(Post != AppliedPostProcess)
        {
            AppliedPostProcess = Post;
            PushPostProcess(World, Post);
        }

        /* The mix: every player's gains multiplied, by bus. */
        std::vector<SequenceBusGain> Gains;
        for(const Player* P : Playing)
        {
            for(const SequenceBusGain& Gain : P->Instance->GetBusGains())
            {
                auto Found = std::find_if(Gains.begin(), Gains.end(), [&](const SequenceBusGain& In) { return In.Bus == Gain.Bus; });
                if(Found == Gains.end()) { Gains.push_back(Gain); }
                else                     { Found->Gain *= Gain.Gain; }
            }
        }
        Gains.erase(std::remove_if(Gains.begin(), Gains.end(), [](const SequenceBusGain& In) { return In.Gain == 1.0f; }), Gains.end());
        for(const SequenceBusGain& Old : AppliedBusGains)
        {
            const bool bKept = std::any_of(Gains.begin(), Gains.end(), [&](const SequenceBusGain& In) { return In.Bus == Old.Bus; });
            if(!bKept && Host.SetBusGain) { Host.SetBusGain(Old.Bus.c_str(), 1.0f, Host.UserData); }
        }
        for(const SequenceBusGain& Gain : Gains)
        {
            const auto Old = std::find_if(AppliedBusGains.begin(), AppliedBusGains.end(), [&](const SequenceBusGain& In) { return In.Bus == Gain.Bus; });
            const bool bChanged = Old == AppliedBusGains.end() || Old->Gain != Gain.Gain;
            if(bChanged && Host.SetBusGain) { Host.SetBusGain(Gain.Bus.c_str(), Gain.Gain, Host.UserData); }
        }
        AppliedBusGains = std::move(Gains);

        /* The subtitle: the started-last player with a line. */
        SequenceSubtitle Line;
        if(GSubtitlesEnabled)
        {
            for(auto It = Playing.rbegin(); It != Playing.rend(); ++It)
            {
                const SequenceSubtitle& Candidate = (*It)->Instance->GetSubtitle();
                if(!Candidate.IsEmpty()) { Line = Candidate; break; }
            }
        }
        if(Line != AppliedSubtitle)
        {
            AppliedSubtitle = Line;
            if(Host.SetSubtitle) { Host.SetSubtitle(Line.Speaker.c_str(), Line.Text.c_str(), Host.UserData); }
        }

        if(bCinema != bAppliedCinematic)
        {
            bAppliedCinematic = bCinema;
            if(Host.SetCinematic) { Host.SetCinematic(bCinema, Host.UserData); }
        }
    }

    // --- Blending in and out ------------------------------------------------------

    GameCore::EntityID SequenceWorld::GetBlendCamera() const
    {
        return Blend.bActive && BlendCamera.IsSet() && World.IsAlive(BlendCamera) ? BlendCamera : GameCore::EntityID();
    }

    bool SequenceWorld::HasReleasedControl(const Player& InPlayerState, const SequencePlayerComponent& InComponent) const
    {
        if(InComponent.ReleaseControl != SCR_BeforeEnd || !InPlayerState.Instance || InComponent.bLoop) { return false; }
        const SequenceAsset& Asset = InPlayerState.Instance->GetAsset();
        const double Left = Asset.FramesToSeconds(double(Asset.Length) - InPlayerState.Frame);
        return Left <= double(InComponent.ReleaseBeforeEnd);
    }

    bool SequenceWorld::ReadShownView(SequenceShot& OutShot)
    {
        if(Blend.bActive && BlendCamera.IsSet() && ReadCameraView(World, BlendCamera, OutShot)) { return true; }
        return AppliedCamera.IsSet() && ReadCameraView(World, AppliedCamera, OutShot);
    }

    GameCore::EntityID SequenceWorld::ProbeDefaultView()
    {
        if(!Host.SetViewTarget || !Host.GetViewTarget) { return GameCore::EntityID(); }
        Host.SetViewTarget(GameCore::EntityID(), Host.UserData);
        const GameCore::EntityID Default = Host.GetViewTarget(Host.UserData);
        if(Blend.bActive && BlendCamera.IsSet() && World.IsAlive(BlendCamera)) { Host.SetViewTarget(BlendCamera, Host.UserData); }
        return Default;
    }

    void SequenceWorld::StartBlend()
    {
        if(!BlendCamera.IsSet() || !World.IsAlive(BlendCamera))
        {
            GameCore::Entity Made = World.CreateEntity("Sequence Blend Camera");
            Made.AddComponent(GameCore::TransformComponent());
            RenderCore::CameraComponent Camera;
            Camera.Projection = RenderCore::CP_Perspective;
            Made.AddComponent(Camera);
            BlendCamera = Made.GetID();
        }
        Blend.bFresh = true;
        if(!PoseBlend())
        {
            EndBlend(true);
            return;
        }
        if(Host.SetViewTarget) { Host.SetViewTarget(BlendCamera, Host.UserData); }
    }

    bool SequenceWorld::PoseBlend()
    {
        SequenceShot From = Blend.FromView;
        if(Blend.FromEntity.IsSet() && !ReadCameraView(World, Blend.FromEntity, From)) { return false; }
        SequenceShot To;
        if(!ReadCameraView(World, Blend.ToEntity, To))
        {
            /* The view to give back went (an avatar respawned): the host's default now. */
            if(!Blend.bOut) { return false; }
            Blend.ToEntity   = ProbeDefaultView();
            Blend.bToDefault = true;
            if(!ReadCameraView(World, Blend.ToEntity, To)) { return false; }
        }
        const float T = Blend.Seconds > 0.0f ? SequenceBlendEase(Blend.Ease, Blend.Elapsed / Blend.Seconds) : 1.0f;
        const SequenceShot View = BlendShots(From, To, T);
        GameCore::Entity Camera = World.GetEntity(BlendCamera);
        Camera.SetWorldPose(View.Pose);
        if(RenderCore::CameraComponent* Component = Camera.GetComponent<RenderCore::CameraComponent>())
        {
            Component->FovY  = View.FovY;
            Component->ZNear = View.ZNear;
            Component->ZFar  = View.ZFar;
        }
        return true;
    }

    void SequenceWorld::EndBlend(const bool bInHandOver)
    {
        const ViewBlend Ended = Blend;
        Blend = ViewBlend();
        if(bInHandOver && Ended.bActive && Host.SetViewTarget)
        {
            if(Ended.bOut)
            {
                /* As a cut gives back: what it took, or the host's default. */
                const GameCore::EntityID Back = !Ended.bToDefault && Ended.ToEntity.IsSet() && World.IsAlive(Ended.ToEntity)
                    ? Ended.ToEntity : GameCore::EntityID();
                Host.SetViewTarget(Back, Host.UserData);
                bViewTaken = false;
                ViewBefore = GameCore::EntityID();
            }
            else
            {
                Host.SetViewTarget(AppliedCamera, Host.UserData);
            }
        }
        if(BlendCamera.IsSet())
        {
            if(World.IsAlive(BlendCamera)) { World.DestroyEntity(BlendCamera); }
            BlendCamera = GameCore::EntityID();
        }
    }

    void SequenceWorld::TickBlend(const float InDeltaSeconds)
    {
        if(!Blend.bActive) { return; }
        if(!Blend.bFresh) { Blend.Elapsed += InDeltaSeconds; }
        Blend.bFresh = false;
        const bool bHeld = Blend.bOut && Blend.bHoldControl;
        if(Blend.Elapsed >= Blend.Seconds || !PoseBlend())
        {
            EndBlend(true);
            if(bHeld) { ApplyToHost(); }   // cinematic mode may end with it
        }
    }

    void SequenceWorld::Tick(const float InDeltaSeconds)
    {
        /* Players whose entity or component went: released, forgotten. */
        for(auto It = Players.begin(); It != Players.end();)
        {
            const GameCore::EntityID Entity = GameCore::EntityID::Unpack(It->first);
            if(World.IsAlive(Entity) && World.GetEntity(Entity).HasComponent<SequencePlayerComponent>())
            {
                ++It;
                continue;
            }
            Release(It->second);
            It = Players.erase(It);
        }

        /* Gathered first: a sequence may spawn, which may add players. */
        std::vector<GameCore::EntityID> Ids;
        World.Each<SequencePlayerComponent>([&Ids](const GameCore::Entity& InEntity, SequencePlayerComponent&)
        {
            Ids.push_back(InEntity.GetID());
        });

        for(const GameCore::EntityID Id : Ids)
        {
            Player* P = GetPlayer(Id);
            if(!P) { continue; }
            if(IsReplica(Id))
            {
                P->bAutoPlayed = true;   // the server's plays, not this copy
                Follow(Id, *P);
            }
            else if(!P->bAutoPlayed)
            {
                P->bAutoPlayed = true;
                const SequencePlayerComponent* Component = World.GetEntity(Id).GetComponent<SequencePlayerComponent>();
                if(Component->bAutoPlay && P->State == ESequencePlayState::Stopped)
                {
                    Play(Id);
                }
            }
            if(P->State == ESequencePlayState::Playing && P->Instance)
            {
                /* A sequence's first tick shows the frame it starts on: no time passes. */
                const float Delta = P->bFresh ? 0.0f : InDeltaSeconds;
                P->bFresh = false;
                Advance(Id, *P, Delta);
            }
        }
        ApplyToHost();
    }
}
