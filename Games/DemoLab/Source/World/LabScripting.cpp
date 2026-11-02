#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"

#include "World/LabSignals.h"

/*
 * The "Lab" script library: Demo Lab's signals and story banner for Lua and
 * script graphs (GameCore's README, "Scripting"). In Lua it is the global
 * table Lab; in a script graph, the Lab group of the node palette.
 *
 *   Lab.GetSignal("L2.Keycard")          the channel's latched value
 *   Lab.Emit("L2.ArchiveOpen", true)     a signal, as a lever would send it
 *   Lab.ShowBanner("Access denied", 3)   the story's centred message
 *
 * A script hears a signal with Events.On("Lab.Signal", fn(channel, value,
 * source)), or a graph's On Event node named Lab.Signal.
 * Content/Prefabs/Lab/KeycardReader.lua and ValveControl.script.nmgraph use
 * them.
 */
namespace
{
    bool ScriptGetSignal(GameCore::World& InWorld, const char* InChannel)
    {
        return LabSignals::Get(InWorld, InChannel);
    }

    /* Scripts usually emit from inside a "Lab.Signal" listener, and the bus
     * refuses an event re-entering its own broadcast: queued, it goes out at
     * the next LabSignals::Flush (later this frame). */
    void ScriptEmit(GameCore::World& InWorld, const char* InChannel, const bool bInValue)
    {
        LabSignals::BeginHandling(InWorld);
        LabSignals::Emit(InWorld, InChannel, bInValue);
        LabSignals::EndHandling(InWorld);
    }

    /* "Story.Banner" (LabStoryMode): the text, for InSeconds. */
    void ScriptShowBanner(GameCore::World& InWorld, const char* InText, const float InSeconds)
    {
        GameCore::EventArgs Args;
        Args.Args[Args.Count++] = GameCore::EventArg::Make(InText);
        Args.Args[Args.Count++] = GameCore::EventArg::Make(double(InSeconds));
        InWorld.GetEvents().Emit("Story.Banner", Args);
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Lab)
{
    Lib.NativePure<&ScriptGetSignal>("GetSignal", "Channel -> On")
       .Tooltip("The last value emitted on a Demo Lab signal channel in this level; false if never");
    Lib.Native<&ScriptEmit>("Emit", "Channel, Value")
       .Tooltip("Emit a Demo Lab signal (Lab.Signal), as a lever or trigger would; sent later this frame");
    Lib.Native<&ScriptShowBanner>("ShowBanner", "Text, Seconds")
       .Tooltip("Show the story's centred banner for a few seconds");
}
