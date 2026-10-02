#ifndef NIMBLEEDITOR_CONTENTPATHS_H
#define NIMBLEEDITOR_CONTENTPATHS_H

#if WITH_EDITOR

#include <QString>

class QMimeData;

/*
 * Project files as the engine names them: assets by project-relative path
 * ("Content/Props/Crate.nmmesh", staged at the same path beside the exe),
 * materials by base name ("Glow").
 */
namespace EditorContent
{
    /* Project-relative, forward slashes ("Content/Props/Crate.nmmesh"), or
     * "Engine/Content/..." for the engine's own content; empty if outside both. */
    QString ToRuntimePath(const QString& InPath);

    /*
     * The engine's content folder (Engine/Content: its meshes, materials and
     * shaders), absolute, forward slashes: the source tree this editor was
     * built from when it is there, else the copy staged beside the executable.
     * Empty if neither exists. Read-only to the editor; staged at
     * "Engine/Content/..." beside every game.
     */
    QString GetEngineContentRoot();

    /* Whether InPath is the engine content folder or inside it. */
    bool IsEngineContent(const QString& InPath);

    /* Every material's name, sorted: the project's Materials folder's and the
     * engine's, from their graphs and .material files. */
    QStringList ListMaterialNames();

    /* The material a file names: "Glow" for Glow.material.nmgraph,
     * Glow.material or Glow.mtl.glsl. Empty otherwise. */
    QString MaterialNameFor(const QString& InPath);

    /* The single local file in a drag, else empty. */
    QString SingleDroppedFile(const QMimeData* InData);

    /* Copies the asset beside the exe and invalidates caches, so the viewport
     * shows it next frame. */
    void StageAsset(const QString& InPath);

    /* Removes the staged copy. */
    void UnstageAsset(const QString& InPath);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONTENTPATHS_H
