#include "Public/NimbleEngine/SequenceFacade.h"

#include "Public/GameCore/World.h"
#include "Public/Sequencer/SequenceWorld.h"

/* SequenceFacade.h has the contract. */

namespace NimbleEngine
{
    GameCore::EntityID PlaySequence(GameCore::World& InWorld, const char* InPath, const bool bInLoop)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players ? Players->PlaySequence(InPath, bInLoop) : GameCore::EntityID();
    }

    bool StartSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Play(InPlayer);
    }

    bool PauseSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Pause(InPlayer);
    }

    bool ResumeSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Resume(InPlayer);
    }

    bool StopSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Stop(InPlayer);
    }

    bool SkipSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Skip(InPlayer);
    }

    bool SeekSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer, const double InSeconds)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Seek(InPlayer, InSeconds);
    }

    bool BindSequence(GameCore::World& InWorld, const GameCore::EntityID InPlayer, const char* InBindingId, const GameCore::EntityID InEntity)
    {
        Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->Bind(InPlayer, InBindingId, InEntity);
    }

    bool IsSequencePlaying(const GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        const Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players && Players->IsPlaying(InPlayer);
    }

    double GetSequenceTime(const GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        const Sequencer::SequenceWorld* Players = Sequencer::SequenceWorld::Find(&InWorld);
        return Players ? Players->GetTime(InPlayer) : 0.0;
    }
}
