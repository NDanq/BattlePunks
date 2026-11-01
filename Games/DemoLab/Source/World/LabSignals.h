#ifndef DEMOLAB_LABSIGNALS_H
#define DEMOLAB_LABSIGNALS_H

#include "Public/GameCore/EntityID.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GameCore { class World; struct EventArgs; }

/*
 * Named signals (TDD §7.5): "the lever opens the gate" is wired by channel
 * name, because the Details panel has no EntityID picker. A signal is the
 * World event "Lab.Signal" with (channel, value, source):
 *
 *   C++        LabSignals::Emit(World, "Vault.Open", true, Lever)
 *   Lua        Events.Emit("Lab.Signal", "Vault.Open", true)
 *   Sequencer  an Event key: event="Lab.Signal" arg0="Vault.Open" arg1="true"
 *
 * Signals are gameplay: the authority's listeners act on them, and what they
 * change replicates. The last value per channel is latched, so a gate placed
 * after its signal still opens.
 */
namespace LabSignals {
    constexpr const char* EVENT = "Lab.Signal";

    void Emit(GameCore::World& InWorld, const char* InChannel, bool bInValue, GameCore::EntityID InSource = GameCore::EntityID());

    /* Reads a "Lab.Signal" event's args; false if they are not a signal. A
     * missing value counts as true (an Event key with only arg0). */
    bool Read(const GameCore::EventArgs& InArgs, const char*& OutChannel, bool& bOutValue);

    /* The last value emitted on InChannel in InWorld; false if never. */
    bool Get(GameCore::World& InWorld, const char* InChannel);
    bool WasEmitted(GameCore::World& InWorld, const char* InChannel);

    /* Called by the mode on every "Lab.Signal", so the latch sees signals
     * from Lua and the Sequencer too. */
    void Latch(GameCore::World& InWorld, const char* InChannel, bool bInValue);
    /* Forget InWorld's latches (the mode's OnLoad and OnUnload). */
    void Reset(GameCore::World& InWorld);

    /* A signal emitted while one is being handled (a listener reacting to it)
     * cannot re-enter the event's broadcast: it waits in a queue the mode
     * flushes once the handling is over. */
    void BeginHandling(GameCore::World& InWorld);
    void EndHandling(GameCore::World& InWorld);
    void Flush(GameCore::World& InWorld);

    /* Every latch, for a save (a story checkpoint), and back. */
    std::vector<std::pair<std::string, bool>> Snapshot(GameCore::World& InWorld);
    void Restore(GameCore::World& InWorld, const std::vector<std::pair<std::string, bool>>& InLatches);
}

#endif //DEMOLAB_LABSIGNALS_H
