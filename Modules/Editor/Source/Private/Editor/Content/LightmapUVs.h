#ifndef NIMBLEEDITOR_LIGHTMAPUVS_H
#define NIMBLEEDITOR_LIGHTMAPUVS_H

#if WITH_EDITOR

#include <string>

#include "Public/RenderCore/MeshAsset.h"

/*
 * Lightmap UVs (MeshData::UV1) for a mesh that has none: xatlas
 * (External/xatlas) cuts it into charts and packs them into 0-1 without
 * overlap, leaving a gutter between charts for a lightmap of at least
 * MIN_LIGHTMAP_RESOLUTION texels across (baked lighting; RENDERPIPELINE.md).
 *
 * Seams split vertices, so the mesh may come back with more of them: every
 * vertex keeps its index, the copies a seam needs are appended (every other
 * stream copied to them), and the indices are remapped triangle for
 * triangle, so sections keep their ranges. Widget-free.
 */
namespace EditorContent
{
    /* The smallest lightmap a mesh's UV1 keeps its charts apart in. */
    constexpr int MIN_LIGHTMAP_RESOLUTION = 32;

    /* Fills InOutMesh.UV1 (replacing any). False, the mesh untouched, with
     * OutError if xatlas cannot chart it. */
    bool GenerateLightmapUVs(RenderCore::MeshData& InOutMesh, std::string& OutError);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_LIGHTMAPUVS_H
