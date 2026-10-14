#ifndef RENDERCORE_PROCEDURALSPAWNERCOMPONENT_H
#define RENDERCORE_PROCEDURALSPAWNERCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

namespace RenderCore
{
    /* Where a spawner finds its candidate points. */
    enum EProceduralSource : uint8_t
    {
        PGS_SplinePath   = 0,   // along this entity's Spline, across its Width
        PGS_SplineArea   = 1,   // inside this entity's closed Spline
        PGS_Box          = 2,   // inside Box Extent around this entity
        PGS_MeshSurface  = 3,   // over this entity's own Render mesh, on its triangles
    };

    /* What the points are dropped onto, straight down in the world. */
    enum EProceduralProject : uint8_t
    {
        PGP_None    = 0,   // stay where the source put them
        PGP_Terrain = 1,   // the world's terrains
        PGP_Meshes  = 2,   // the world's static Render meshes
        PGP_All     = 3,   // whichever is higher
    };

    enum : uint32_t
    {
        PROCEDURAL_MESHES_MAX     = 8,
        PROCEDURAL_EXCLUSIONS_MAX = 8,
        PROCEDURAL_LODS           = 3,
        PROCEDURAL_ANY_LAYER      = 0,   // TerrainLayer: 0 any, 1..4 a terrain paint layer 0..3
        PROCEDURAL_NAME_MAX       = 64,  // an exclusion's entity name, including the NUL
    };

    /* One mesh a spawner places; picked per instance by Weight. */
    struct ProceduralMeshEntry
    {
        char  Mesh[RENDER_MESH_PATH_MAX]         = {};
        char  FarMesh[RENDER_MESH_PATH_MAX]      = {};   // LOD 1 and 2; empty: Mesh
        char  Material[RENDER_MATERIAL_PATH_MAX] = {};   // every section's; empty: the mesh's own
        float Weight = 1.0f;
        float Scale  = 1.0f;                             // times the spawner's random scale
    };

    /*
     * "Procedural Spawner": many copies of a few meshes, placed by rules
     * rather than by hand, drawn as instanced meshes (as terrain foliage is,
     * FOLIAGE.md). Fences and lamp posts along a road, a field of grass in a
     * meadow's outline, rocks strewn over a box, moss over a boulder.
     *
     * WHERE (Source): along this entity's Spline (Spline Path: a step every
     * Spacing, across the spline's Width in lanes Spacing apart, moved Path
     * Offset to its right -- and to its left too with Both Sides), inside it
     * (Spline Area: a closed spline, filled on a grid Spacing apart), inside
     * Box Extent (on its local X/Z, a grid Spacing apart), or over this
     * entity's own Render mesh (Mesh Surface: one per Spacing x Spacing of
     * its triangles' area, standing on them). Jitter moves each grid point
     * up to a cell's width (0: a neat grid, 1: as random as it gets).
     *
     * ONTO (Project): dropped straight down onto the world's terrains, its
     * static Render meshes, or both (the higher wins); a point with nothing
     * under it is dropped. Spline sources look Project Range above and below
     * the spline, Box sources inside the box. Mesh Surface never projects.
     *
     * KEPT where: Density (a random fraction), the noise mask (Noise Scale:
     * world units per patch, 0 off; Noise Threshold: how much is bare),
     * ground no steeper than Max Slope, Min/Max Height (world Y), a terrain
     * layer painted at least Layer Threshold (Terrain Layer, terrain hits
     * only), and outside every Exclusions entity's Spline (named: inside a
     * closed one, or within half its Width).
     *
     * EACH INSTANCE: a Meshes entry by Weight, a scale between Scale Min and
     * Max (times the entry's Scale), a random yaw (Random Yaw) or the
     * spline's heading (Align to Path), tilted toward the surface normal by
     * Align to Normal, and pushed in by Sink times its scale.
     *
     * DETERMINISTIC: the same rules, Seed and surfaces give the same
     * instances, and each one comes from its own grid cell's hash -- moving
     * a spline point or painting a terrain changes only the instances near
     * the change. They are not saved: they are made again on load, on a
     * worker thread, whenever the rules, the entity, its splines or the
     * surfaces under it change. Instance Count says how many there are.
     *
     * DRAWING: instances are bucketed into cells Cell Size wide, each culled
     * on its own. Within LOD Distance of the view a cell draws them all;
     * twice as far, LOD 1 Density of them with each entry's Far Mesh;
     * further, LOD 2 Density. Thinning keeps the same ones at every distance.
     * Past Cull Distance (0: never) a cell draws nothing.
     */
    struct RENDERCORE_API ProceduralSpawnerComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        uint8_t  bVisible     = 1;
        uint8_t  Source       = PGS_Box;       // EProceduralSource
        uint8_t  Project      = PGP_All;       // EProceduralProject
        uint8_t  TerrainLayer = PROCEDURAL_ANY_LAYER;
        int32_t  Seed         = 1;

        // === Where ==============================================================
        EngineCore::Vector3 BoxExtent{ 1000.0f, 500.0f, 1000.0f };   // half sizes, entity space (Box)
        float    Spacing      = 200.0f;     // world units between neighbours
        float    Jitter       = 1.0f;       // 0..1 of a cell
        float    ProjectRange = 5000.0f;    // spline sources: searched this far above and below
        float    PathOffset   = 0.0f;       // Spline Path: the lanes' centre, this far right of the curve
        uint8_t  bBothSides   = 0;          // Spline Path: and as far left, mirrored
        uint8_t  Reserved1    = 0;
        uint8_t  Reserved2    = 0;
        uint8_t  Reserved3    = 0;

        // === Kept where =========================================================
        float    Density        = 1.0f;     // 0..1 kept
        float    NoiseScale     = 0.0f;     // world units per patch; 0: no mask
        float    NoiseThreshold = 0.5f;     // 0..1: the bare share
        float    MaxSlope       = 90.0f;    // degrees from up
        float    MinHeight      = -1.0e6f;  // world Y
        float    MaxHeight      =  1.0e6f;
        float    LayerThreshold = 0.5f;     // TerrainLayer's weight needed, 0..1

        char     Exclusions[PROCEDURAL_EXCLUSIONS_MAX][PROCEDURAL_NAME_MAX] = {};   // names of entities with a Spline

        // === Each instance ======================================================
        float    ScaleMin       = 0.8f;
        float    ScaleMax       = 1.2f;
        float    AlignToNormal  = 0.0f;     // 0: upright, 1: along the surface normal
        float    Sink           = 0.0f;     // into the surface, times its scale
        float    ColorVariation = 0.1f;     // each instance darkened by up to this
        uint8_t  bRandomYaw     = 1;
        uint8_t  bAlignToPath   = 0;        // Spline Path: face along the curve (+Z forward)
        uint8_t  bCastShadows   = 1;
        uint8_t  Reserved0      = 0;

        ProceduralMeshEntry Meshes[PROCEDURAL_MESHES_MAX];

        // === Drawing ============================================================
        float    CellSize       = 4000.0f;  // world units a side
        float    LODDistance    = 0.0f;     // 0: one and a half cells
        float    LOD1Density    = 0.5f;
        float    LOD2Density    = 0.25f;
        float    CullDistance   = 0.0f;     // 0: never
        int32_t  MaxInstances   = 200000;

        // === State (this run's, not saved) =======================================
        int32_t  InstanceCount  = 0;        // as of the last generation
        float    GenerateMilliseconds = 0.0f;
    };

    /* ProceduralSpawnerComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetProceduralSpawnerComponentFactory();
}

#endif //RENDERCORE_PROCEDURALSPAWNERCOMPONENT_H
