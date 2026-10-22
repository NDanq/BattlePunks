#ifndef NIMBLEEDITOR_MESHTANGENTS_H
#define NIMBLEEDITOR_MESHTANGENTS_H

#if WITH_EDITOR

#include "Public/RenderCore/MeshAsset.h"

/*
 * Tangents (MeshData::Tangents) for a mesh that has none: MikkTSpace
 * [Mikkelsen 2008] (ThirdParty/Source/mikktspace-3e895b4), the frame normal
 * maps are baked against by Blender, Substance and xNormal, from the
 * positions, normals and UV0.
 *
 * MikkTSpace answers per triangle corner, and two corners of one vertex can
 * disagree (where a mirrored UV island meets its twin, the handedness flips):
 * such a vertex is split, the copy appended with every other stream copied
 * to it and the corner's index remapped, so sections keep their ranges, as
 * GenerateLightmapUVs does. Widget-free.
 */
namespace EditorContent
{
    /* Fills InOutMesh.Tangents (replacing any). False, the mesh untouched, if
     * it has no triangles, or no normal or UV per vertex. */
    bool GenerateMeshTangents(RenderCore::MeshData& InOutMesh);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_MESHTANGENTS_H
