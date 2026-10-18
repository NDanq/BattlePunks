#ifndef NIMBLEEDITOR_ASSETIMPORT_H
#define NIMBLEEDITOR_ASSETIMPORT_H

#if WITH_EDITOR

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "Public/RenderCore/TextureAsset.h"

/*
 * glTF / GLB -> .nmmesh, images -> .nmtexture and, with the Audio module,
 * sound files -> .nmsound. Widget-free and
 * thread-safe (the wizard runs these on a worker). Outputs record their
 * source and options for Reimport. Writes are atomic (QSaveFile).
 */
namespace EditorContent
{
    enum class EImportKind
    {
        None,
        Mesh,       // .gltf, .glb
        Texture,    // any image Qt can read
        Sound       // .wav, .flac, .mp3, .ogg -- only with the Audio module (WITH_AUDIO)
    };

    EImportKind ClassifySource(const QString& InPath);

    /* "Importable Files (*.gltf *.glb *.png ...);;Meshes (...);;Images (...)" */
    QString GetImportFileFilter();

    /* ".nmmesh" / ".nmtexture" / ".nmsound" for a kind; empty for None. */
    QString GetAssetExtension(EImportKind InKind);

    // --- Options ------------------------------------------------------------------

    struct MeshImportOptions
    {
        /* glTF is in metres: 100 makes 1 m = 100 units. */
        float Scale = 100.0f;

        /* glTF is right-handed with +Z toward the viewer; the engine is
         * left-handed with +Z away (RenderView.h). Both are +Y up: z flips,
         * a mirror, so the winding flips with it, and the front faces the
         * default camera. */
        bool bConvertCoordinates = true;

        /* For primitives without normals (else they draw dark). */
        bool bGenerateNormals = true;

        /* Lightmap UVs (UV1, LightmapUVs.h) for a mesh whose file has no
         * TEXCOORD_1; without them it is not lightmapped. */
        bool bGenerateLightmapUVs = true;

        /* One .nmmesh for the scene (primitives as sections), or one per mesh. */
        bool bCombineMeshes = true;

        /* Import the images the glTF refers to as .nmtexture beside the mesh. */
        bool bImportTextures = true;

        /* An untextured material's base colour baked into the vertex colours,
         * so the mesh draws in its colours with DefaultLit. Recorded settings
         * without it reimport as before (off). */
        bool bBakeMaterialColors = true;

        /* A skinned file's skeleton (.nmskel), its meshes skinned, and its
         * animations as .nmanim clips (ANIMATION.md). */
        bool  bImportAnimations   = true;
        /* Frames per second the clips are resampled at. */
        float AnimationSampleRate = 30.0f;

        /* A .nmskel (runtime path) the file's bones are already in -- the
         * hundredth clip of one rig: no skeleton is written, the clips (and a
         * skinned mesh) name this one. Every bone of the file must be in it;
         * an import whose bones are not fails, naming them. Empty: none. */
        QString ExistingSkeleton;

        /* A .nmskel (runtime path) to play the file's clips on: an
         * auto-mapped, arm-aligned .nmretarget from the file's skeleton to
         * it is written beside the clips (<Base>_to_<Target>.nmretarget;
         * an existing one gains or replaces the source). Empty: none. */
        QString CreateRetargetTo;

        QString ToSettings() const;
        static MeshImportOptions FromSettings(const QString& InSettings);
    };

    struct TextureImportOptions
    {
        bool                       bSRGB         = true;
        RenderCore::ETextureFilter Filter        = RenderCore::ETextureFilter::Linear;
        RenderCore::ETextureWrap   Wrap          = RenderCore::ETextureWrap::Repeat;
        int                        MaxDimension  = 4096;   // larger images are scaled down to fit
        bool                       bFlipVertically = false;
        RenderCore::ETextureMipGen      MipGen      = RenderCore::ETextureMipGen::Box;   // a settings string without it: None
        RenderCore::ETextureCompression Compression = RenderCore::ETextureCompression::None;
        /* A name that says data -- normal, rough, metal, AO, height, mask... -- imports as linear,
         * whatever bSRGB says. Recorded settings hold the result, so a reimport does not re-guess. */
        bool                       bDetectDataMaps = true;

        /* These options for InName: bSRGB off when bDetectDataMaps and the name says data. */
        TextureImportOptions ResolvedFor(const QString& InName) const;
        static bool IsDataMapName(const QString& InName);

        QString ToSettings() const;
        static TextureImportOptions FromSettings(const QString& InSettings);
    };

    // --- Importing ------------------------------------------------------------------

    /* An image a glTF uses: a file beside it, or bytes inside it (GLB, data URI). */
    struct ReferencedImage
    {
        QString    Name;          // the image's name, else its file's base name, else "<glTF name>_Image<n>"
        QString    SourcePath;    // a file on disk; empty when embedded
        QByteArray Embedded;      // the encoded image when it has no file
        int        Index = -1;    // its index in the glTF's images
    };

    struct ImportResult
    {
        bool                   bOK = false;
        QStringList            Written;    // absolute paths of the assets written
        QString                Message;    // one line: what happened, or why not
        QStringList            Warnings;   // what was skipped or guessed
        QList<ReferencedImage> Images;     // ImportMesh: what the glTF refers to
    };

    /* The engine's content folder (ContentPaths' GetEngineContentRoot), set
     * once at startup, before any import: what is written inside it names
     * what it refers to -- a mesh its skeleton, a clip or mesh its source --
     * as "Engine/Content/...", not by absolute path. Empty: no engine content. */
    void SetImportEngineContentRoot(const QString& InRoot);

    /* Lists InGltfPath's images without importing. */
    bool FindReferencedImages(const QString& InGltfPath, QList<ReferencedImage>& OutImages, QString& OutError);

    /* Writes InBaseName.nmmesh (or InBaseName_<mesh>.nmmesh each), plus
     * textures if enabled. InProjectRoot relativizes the recorded source. */
    ImportResult ImportMesh(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                            const MeshImportOptions& InOptions, const TextureImportOptions& InTextureOptions,
                            const QString& InProjectRoot);

    /* An image file into InDestFolder/InBaseName.nmtexture. */
    ImportResult ImportTexture(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                               const TextureImportOptions& InOptions, const QString& InProjectRoot);

    /* A referenced image (ReferencedImage) the same way -- from its file or its bytes. */
    ImportResult ImportReferencedImage(const ReferencedImage& InImage, const QString& InGltfPath,
                                       const QString& InDestFolder, const QString& InBaseName,
                                       const TextureImportOptions& InOptions, const QString& InProjectRoot);

    /* A sound file into InDestFolder/InBaseName.nmsound, cooked by its
     * default settings -- or, replacing a sound, by that sound's settings, so
     * what an author set survives a new source. Fails without the module. */
    ImportResult ImportSound(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                             const QString& InProjectRoot);

    /* From the recorded source and options; fails if none or the source is
     * gone. Meshes reimport without their textures; a baked clip
     * (BakeRetargetClip) is baked again. */
    ImportResult Reimport(const QString& InAssetPath, const QString& InProjectRoot);

    /* The clip InClipFile played through the retarget InRetargetFile, baked
     * into an ordinary .nmanim on the retarget's target skeleton at InOutFile
     * (RenderCore's BakeRetargetedClip): no runtime cost, and it plays where
     * no retarget is set. What the editor set on an existing InOutFile --
     * notifies, looping, root motion -- is kept. Its import info records the
     * clip and the retarget ("BakedVia"), so Reimport bakes it again. */
    ImportResult BakeRetargetClip(const QString& InClipFile, const QString& InRetargetFile, const QString& InOutFile,
                                  const QString& InProjectRoot);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ASSETIMPORT_H
