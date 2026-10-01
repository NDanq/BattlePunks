#ifndef NIMBLEEDITOR_ASSETIMPORT_H
#define NIMBLEEDITOR_ASSETIMPORT_H

#if WITH_EDITOR

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "Public/RenderCore/TextureAsset.h"

/*
 * glTF / GLB -> .nmmesh and images -> .nmtexture. Widget-free and
 * thread-safe (the wizard runs these on a worker). Outputs record their
 * source and options for Reimport. Writes are atomic (QSaveFile).
 */
namespace EditorContent
{
    enum class EImportKind
    {
        None,
        Mesh,       // .gltf, .glb
        Texture     // any image Qt can read
    };

    EImportKind ClassifySource(const QString& InPath);

    /* "Importable Files (*.gltf *.glb *.png ...);;Meshes (...);;Images (...)" */
    QString GetImportFileFilter();

    /* ".nmmesh" / ".nmtexture" for a kind; empty for None. */
    QString GetAssetExtension(EImportKind InKind);

    // --- Options ------------------------------------------------------------------

    struct MeshImportOptions
    {
        /* glTF is in metres: 100 makes 1 m = 100 units. */
        float Scale = 100.0f;

        /* glTF is +Y up; the engine is +Y down. Rotates 180 degrees about X so
         * up stays up and the front faces the default camera. */
        bool bConvertCoordinates = true;

        /* For primitives without normals (else they draw dark). */
        bool bGenerateNormals = true;

        /* One .nmmesh for the scene (primitives as sections), or one per mesh. */
        bool bCombineMeshes = true;

        /* Import the images the glTF refers to as .nmtexture beside the mesh. */
        bool bImportTextures = true;

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

        QString ToSettings() const;
        static TextureImportOptions FromSettings(const QString& InSettings);
    };

    // --- Importing ------------------------------------------------------------------

    /* An image a glTF uses: a file beside it, or bytes inside it (GLB, data URI). */
    struct ReferencedImage
    {
        QString    Name;          // the image's name, else its file's base name, else "Image<n>"
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

    /* From the recorded source and options; fails if none or the source is
     * gone. Meshes reimport without their textures. */
    ImportResult Reimport(const QString& InAssetPath, const QString& InProjectRoot);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ASSETIMPORT_H
