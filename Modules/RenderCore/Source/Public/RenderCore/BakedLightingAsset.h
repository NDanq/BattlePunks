#ifndef RENDERCORE_BAKEDLIGHTINGASSET_H
#define RENDERCORE_BAKEDLIGHTINGASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmlighting -- a world's baked lighting, made by the editor's Build > Bake
 * Lightmaps from its static lights and static RenderComponents, and named by
 * the world (WorldLightingSettings::BakedData). See RENDERPIPELINE.md,
 * "Baked lighting". An EngineCore::Archive of chunks:
 *
 *   NBLT v1                the file
 *     BLHD v1              the hash of what was baked; whether the sky was (a
 *                          static SkyLight); atlas, entry and probe counts
 *     BLAT v1              the lightmap atlases: runtime-relative .nmtexture paths (RGBE8)
 *     BLMP v1              per lightmapped entity: its GUID, its atlas, and the
 *                          scale and offset taking its mesh's UV1 into that atlas
 *     BLPR v1              the light probes: a grid over the static entities'
 *                          bounds, each probe an ambient cube
 *
 * Every value is "diffuse radiance": irradiance over pi, what a white
 * Lambertian surface sends back, so a surface adds Value * Albedo -- as the
 * sky/ground ambient is added (Lighting.glsl).
 */
namespace RenderCore
{
    struct BakedLightmapEntry
    {
        uint64_t GuidHi = 0;
        uint64_t GuidLo = 0;
        uint32_t Atlas  = 0;
        float    ScaleOffset[4] = { 1.0f, 1.0f, 0.0f, 0.0f };   // atlas UV = UV1 * xy + zw
    };

    /*
     * An ambient cube: the diffuse radiance a surface facing each axis gets,
     * in the order +X, -X, +Y, -Y, +Z, -Z, linear RGB. A normal N takes
     * N.x^2 of the +X or -X face (by its sign), and so on -- every term is
     * positive, so it survives RGBE and blends without ringing.
     */
    struct BakedAmbientCube
    {
        float Faces[6][3] = {};

        /* For a unit normal. */
        void Evaluate(const float (&InNormal)[3], float (&OutRgb)[3]) const;
    };

    /* Probes at Origin + (i, j, k) * Spacing, x fastest; a probe inside
     * geometry is not Valid and is left out of the blend. */
    struct BakedProbeGrid
    {
        float                          Origin[3] = { 0.0f, 0.0f, 0.0f };
        float                          Spacing   = 0.0f;
        uint32_t                       Counts[3] = { 0, 0, 0 };
        std::vector<BakedAmbientCube>  Probes;
        std::vector<uint8_t>           Valid;

        bool IsEmpty() const { return Probes.empty(); }

        /* The trilinear blend of the eight probes around InPoint (clamped to
         * the grid), skipping invalid ones. False if every one is. */
        bool Sample(const float (&InPoint)[3], BakedAmbientCube& OutCube) const;
    };

    struct BakedLightingData
    {
        uint64_t                        InputHash = 0;
        bool                            bSkyBaked = false;   // else the sky's ambient still lights every surface at run time
        std::vector<std::string>        Atlases;
        std::vector<BakedLightmapEntry> Entries;
        BakedProbeGrid                  Probes;

        /* The entry for an entity's GUID; null if it has none. Linear. */
        const BakedLightmapEntry* FindEntry(uint64_t InGuidHi, uint64_t InGuidLo) const;
    };

    /* Both directions. False, logged, for an invalid file. */
    RENDERCORE_API bool SerializeBakedLighting(EngineCore::Archive& Ar, BakedLightingData& InOutData);

    /* Through the file provider, so runtime-relative paths work. */
    RENDERCORE_API bool LoadBakedLightingFile(const std::string& InPath, BakedLightingData& OutData);

#if WITH_EDITOR
    /* Creates folders. */
    RENDERCORE_API bool SaveBakedLightingFile(const std::string& InPath, BakedLightingData& InData);
#endif

    /*
     * Loaded bakes by path, as MeshAssetCache: shared by the game and render
     * threads, thread-safe, a failed load warned once and remembered until
     * Invalidate -- how a new bake reaches the renderer.
     */
    class RENDERCORE_API BakedLightingCache
    {
    public:
        /* The bake, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const BakedLightingData> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);

        /* Bumped by every Invalidate. */
        static uint64_t GetGeneration();
    };

    /* Where a world's bake goes: beside its entities, "Content/Levels/X.nmworld"
     * -> "Content/Levels/X/Lighting/". Ends in a slash; "" for a path without
     * the .nmworld extension. */
    RENDERCORE_API std::string BakedLightingFolderFor(const std::string& InWorldPath);
}

#endif //RENDERCORE_BAKEDLIGHTINGASSET_H
