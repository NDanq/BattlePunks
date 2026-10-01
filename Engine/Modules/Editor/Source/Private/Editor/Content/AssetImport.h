#ifndef NIMBLEEDITOR_ASSETIMPORT_H
#define NIMBLEEDITOR_ASSETIMPORT_H

#if WITH_EDITOR

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "Public/RenderCore/TextureAsset.h"

/*
 * Turning outside files into engine assets: glTF / GLB meshes into .nmmesh,
 * images into .nmtexture (RenderCore's MeshAsset.h / TextureAsset.h). No
 * widgets, and nothing that must run on the UI thread -- the Import Wizard
 * runs these on a worker, and NimbleEditorImportTests calls them directly.
 *
 * Every output records where it came from and with which options (its ISRC
 * chunk), so Reimport can do it again without asking.
 *
 * Writes are all-or-nothing: an asset is serialized in memory and saved
 * through QSaveFile, so a failed import leaves any old file as it was.
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
        /* Multiplies every position: glTF is in metres, so 100 makes a
         * one-metre box a hundred engine units. */
        float Scale = 100.0f;

        /* glTF is +Y up and faces +Z; the engine is +Y down with the camera
         * looking down +Z. On: rotate the model 180 degrees about X, so up
         * stays up and the model's front faces the default camera. */
        bool bConvertCoordinates = true;

        /* Smooth normals for primitives that have none (else they are flat
         * zero, and draw unlit-dark). */
        bool bGenerateNormals = true;

        /* One .nmmesh for the whole scene (each primitive a section), or one
         * per glTF mesh, named after it. */
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

    /* The images InGltfPath refers to, without importing anything -- for the
     * wizard to list. False, with OutError, if it cannot be read. */
    bool FindReferencedImages(const QString& InGltfPath, QList<ReferencedImage>& OutImages, QString& OutError);

    /* InSource (glTF / GLB) into InDestFolder/InBaseName.nmmesh -- or, with
     * bCombineMeshes off, InBaseName_<mesh>.nmmesh per glTF mesh. With
     * bImportTextures, also each referenced image, using InTextureOptions.
     * InProjectRoot makes the recorded source path relative. */
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

    /* An existing .nmmesh / .nmtexture imported again from the source and
     * options it records. Fails, saying why, if it records none or the
     * source is gone. A combined mesh reimports alone, without its textures. */
    ImportResult Reimport(const QString& InAssetPath, const QString& InProjectRoot);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ASSETIMPORT_H
