#ifndef NIMBLEEDITOR_GLTFIMPORTER_H
#define NIMBLEEDITOR_GLTFIMPORTER_H

#if WITH_EDITOR

#include <QList>
#include <QString>
#include <QStringList>

#include "Public/RenderCore/MeshAsset.h"
#include "Private/Editor/Content/AssetImport.h"

/*
 * The glTF side of AssetImport: cgltf (External/cgltf-1.15) reads the file and
 * its buffers, and this turns the scene into MeshData -- node transforms
 * baked in, InOptions applied. Private to Content/; AssetImport writes files.
 */
namespace EditorContent
{
    struct GltfMesh
    {
        QString              Name;   // the glTF mesh's (or node's) name; "Mesh" when combined
        RenderCore::MeshData Data;   // bounds computed; no import info yet
    };

    /* With InOptions.bCombineMeshes, OutMeshes has one entry for the whole
     * scene; otherwise one per node with a mesh, names made unique. False,
     * with OutError, if the file cannot be read or holds no triangles. */
    bool LoadGltf(const QString& InPath, const MeshImportOptions& InOptions, QList<GltfMesh>& OutMeshes,
                  QList<ReferencedImage>& OutImages, QStringList& OutWarnings, QString& OutError);

    /* Only the images -- what FindReferencedImages answers with. */
    bool LoadGltfImages(const QString& InPath, QList<ReferencedImage>& OutImages, QString& OutError);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GLTFIMPORTER_H
