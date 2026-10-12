#ifndef NIMBLEEDITOR_GLTFIMPORTER_H
#define NIMBLEEDITOR_GLTFIMPORTER_H

#if WITH_EDITOR

#include <QList>
#include <QString>
#include <QStringList>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Private/Editor/Content/AssetImport.h"

/*
 * The glTF side of AssetImport: cgltf (ThirdParty/Source/cgltf-1.15) reads the file and
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

    struct GltfClip
    {
        QString                  Name;   // the glTF animation's name, else "Anim<n>"; unique
        RenderCore::AnimClipData Data;   // no skeleton path or import info yet
    };

    /*
     * What a skinned file adds (ANIMATION.md): one skeleton -- every skin's
     * joints and their ancestors -- and its animations, resampled. The
     * skinned meshes' Skeleton paths are left empty for the caller, which
     * knows where the skeleton is written.
     */
    struct GltfAnimationSet
    {
        bool                     bHasSkeleton = false;
        RenderCore::SkeletonData Skeleton;
        QList<GltfClip>          Clips;
    };

    /* One mesh (combined) or one per mesh node, uniquely named. False if
     * unreadable or triangle-free. OutAnimation, if given, receives the
     * skeleton and clips (InOptions.bImportAnimations); without it, skins
     * are ignored and meshes import static, as before. */
    bool LoadGltf(const QString& InPath, const MeshImportOptions& InOptions, QList<GltfMesh>& OutMeshes,
                  QList<ReferencedImage>& OutImages, QStringList& OutWarnings, QString& OutError,
                  GltfAnimationSet* OutAnimation = nullptr);

    /* Only the images -- what FindReferencedImages answers with. */
    bool LoadGltfImages(const QString& InPath, QList<ReferencedImage>& OutImages, QString& OutError);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GLTFIMPORTER_H
