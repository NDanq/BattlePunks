#ifndef RENDERCORE_TERRAINCOMPONENT_H
#define RENDERCORE_TERRAINCOMPONENT_H

#include <cstdint>
#include <string>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Terrain.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

namespace RenderCore
{
    /*
     * "Terrain". Draws the heightfield named by Terrain (a runtime-relative
     * .nmterrain, GameCore's Terrain.h) from its entity's origin: a grid
     * Size x Size on X and Z, heights up +Y, placed by the Transform.
     *
     * Drawn as chunks of TERRAIN_CHUNK_QUADS cells, each culled on its own
     * and rebuilt only when its samples change -- the editor's brushes edit
     * the shared TerrainAssetCache copy, and the proxy picks the change up
     * the next frame, whether the entity is Static or not.
     *
     * Material is a material graph like a Render component's slot. Its
     * vertices carry the four paint weights as Vertex Color (R G B A = layers
     * 0 1 2 3) and a world-tiled UV0 (one tile per UVTileSize units), so a
     * terrain material blends four textures by Vertex Color; the engine's
     * Materials/Terrain.material does. Empty: DefaultLit.
     *
     * Collision is separate, as for meshes: give the entity a Rigid Body
     * (Static, shape Height Field or Auto) for physics. Ground queries
     * (NimbleEngine's SampleGround, the editor's picking) read the terrain
     * itself while Walkable is set.
     */
    struct RENDERCORE_API TerrainComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        char    Terrain[GameCore::TERRAIN_PATH_MAX] = {};
        char    Material[RENDER_MATERIAL_PATH_MAX]  = {};
        uint8_t bVisible     = 1;
        uint8_t bCastShadows = 1;
        uint8_t bOccluder    = 1;   // hides what is behind hills from occlusion culling
        uint8_t bWalkable    = 1;   // SampleGround and editor picking hit it
        float   UVTileSize   = 400.0f;   // world units per texture tile (UV0)
    };

    /* TerrainComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetTerrainComponentFactory();

    /* The MeshAssetCache path a terrain proxy stores chunk (InX, InZ) of
     * entity InOwner under: "@terrain/<owner>/<x>_<z>". Never a file. */
    RENDERCORE_API std::string GetTerrainChunkMeshPath(GameCore::EntityID InOwner, uint32_t InX, uint32_t InZ);

    /* Builds chunk (InX, InZ) of InData as a mesh in terrain space: positions,
     * smooth normals, UV0 tiled every InUVTileSize units, the paint weights
     * as colours, the cells split as GameCore's TerrainData describes. Locks
     * InData. False if the chunk is out of range. */
    RENDERCORE_API bool BuildTerrainChunkMesh(const GameCore::TerrainData& InData, uint32_t InX, uint32_t InZ,
                                              float InUVTileSize, struct MeshData& OutMesh);
}

#endif //RENDERCORE_TERRAINCOMPONENT_H
