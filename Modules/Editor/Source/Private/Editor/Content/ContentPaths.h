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
    /* Project-relative, forward slashes; empty if outside. */
    QString ToRuntimePath(const QString& InPath);

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
