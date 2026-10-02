#ifndef SANDBOX_SANDBOXSCRIPTING_H
#define SANDBOX_SANDBOXSCRIPTING_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/ScriptStruct.h"
#include "Public/Math/Vector.h"

/*
 * What Sandbox exposes to scripts, declared in the game module rather than the
 * engine (SandboxScripting.cpp):
 *
 *   PickupStats              a game-defined value type (a script struct)
 *   SandboxPickupComponent   a game-defined component, "Pickup" in Lua and in
 *                            script graphs, with fields and a Collect method
 *   the "Sandbox" library    functions, variables and typed events
 *
 * The editor reads both when it loads this module, so they are nodes in any
 * script graph (Scripts/Pickup.script.nmgraph, Scripts/ScoreBoard.script.nmgraph),
 * and the game reads them when Play loads it.
 */

/* A game-defined value type for scripts: "PickupStats" in Lua (two values,
 * Value then ID) and a pin type with Make / Break nodes in script graphs. */
struct PickupStats
{
    GAMECORE_SCRIPT_STRUCT_BODY()

    int32_t Value = 1;   // the score it is worth
    int32_t ID    = 0;   // which pickup, in spawn order from 1
};

/* A collectible that bobs around Home until Marker comes within the library's
 * PickupRadius. Spawned by Sandbox.SpawnPickup; driven by Scripts/Pickup.lua. */
struct SandboxPickupComponent
{
    GAMECORE_COMPONENT_BODY()

    EngineCore::Vector3 Home;               // where it bobs around
    PickupStats         Stats;              // GetStats / SetStats in scripts
    float               BobHeight  = 8.0f;  // pixels above and below Home
    uint8_t             bCollected = 0;     // set once, by Collect

    /* Ordinary C++, bound to Lua and the graph as they are (Bind.Native). */

    /* Its Value the first time, 0 after: a pickup two scripts reach in the
     * same frame is not scored twice. */
    int32_t Collect();

    /* Home, raised or lowered by the bob at InSeconds. */
    EngineCore::Vector3 BobbedPosition(double InSeconds) const;
};

namespace SandboxScripting
{
    /* The library's read-only Elapsed, advanced by the game mode. */
    void SetElapsedSeconds(double InSeconds);

    /* The library's Score, for the game mode's report and the game state's
     * save. SetScore does not emit ScoreChanged: it restores, it does not score. */
    int32_t GetScore();
    void    SetScore(int32_t InScore);
}

#endif //SANDBOX_SANDBOXSCRIPTING_H
