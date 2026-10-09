#ifndef GAMECORE_TERRAINBRUSH_H
#define GAMECORE_TERRAINBRUSH_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Terrain.h"

/*
 * The terrain editing operations: one brush dab on a TerrainData. Here
 * rather than in the editor so tests, the console and tools share them; the
 * editor's Terrain tools call ApplyTerrainBrush once a frame while a stroke
 * is held. Rates are per second, scaled by DeltaSeconds, so a stroke does
 * the same work at any frame rate.
 */
namespace GameCore
{
    enum ETerrainTool : uint8_t
    {
        TT_Raise         = 0,   // up, by Rate a second at full strength (Lower: Rate < 0)
        TT_Smooth        = 1,   // toward the 3x3 average
        TT_Flatten       = 2,   // toward TargetHeight
        TT_Noise         = 3,   // value noise, up and down, by Rate
        TT_SetHeight     = 4,   // straight to TargetHeight where the brush is full
        TT_Paint         = 5,   // Layer's weight up, the others down: weights keep summing to one
        TT_SmoothWeights = 6,   // the weights toward their 3x3 average
        TT_Count
    };

    enum ETerrainFalloff : uint8_t
    {
        TF_Smooth = 0,   // smoothstep from the centre to the edge
        TF_Linear = 1,
        TF_Sphere = 2,   // a dome: full most of the way, falling fast at the edge
        TF_Tip    = 3,   // a spike: strongest at the centre only
        TF_Count
    };

    struct TerrainBrush
    {
        float   X        = 0.0f;     // centre, terrain space
        float   Z        = 0.0f;
        float   Radius   = 500.0f;   // world units
        float   Strength = 0.5f;     // 0..1
        uint8_t Falloff  = TF_Smooth;
    };

    struct TerrainBrushOp
    {
        uint8_t  Tool         = TT_Raise;
        float    DeltaSeconds = 1.0f / 60.0f;
        float    Rate         = 400.0f;   // Raise / Noise: units a second at full strength
        float    TargetHeight = 0.0f;     // Flatten / SetHeight
        uint8_t  Layer        = 0;        // Paint
        float    NoiseScale   = 800.0f;   // Noise: the bumps' size, world units
        uint32_t NoiseSeed    = 1;
    };

    /* How much of the brush reaches InDistance from its centre: 1 at the
     * centre, 0 at Radius and beyond. */
    GAMECORE_API float GetTerrainBrushWeight(const TerrainBrush& InBrush, float InDistance);

    /* One dab: locks InData, changes the samples under the brush, marks
     * them dirty. Returns the samples it changed (empty: none). */
    GAMECORE_API TerrainRect ApplyTerrainBrush(TerrainData& InData, const TerrainBrush& InBrush, const TerrainBrushOp& InOp);

    /* "Raise", "Smooth", ... and back; nullptr / TT_Count when unknown. */
    GAMECORE_API const char* GetTerrainToolName(uint8_t InTool);
    GAMECORE_API uint8_t     FindTerrainTool(const char* InName);
    GAMECORE_API const char* GetTerrainFalloffName(uint8_t InFalloff);
    GAMECORE_API uint8_t     FindTerrainFalloff(const char* InName);
}

#endif //GAMECORE_TERRAINBRUSH_H
