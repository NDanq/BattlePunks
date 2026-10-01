#ifndef NIMBLEEDITOR_CONTENTPATHS_H
#define NIMBLEEDITOR_CONTENTPATHS_H

#if WITH_EDITOR

#include <QString>

class QMimeData;

/*
 * Project files as the engine names them. A RenderComponent's Mesh is a
 * runtime-relative path -- "Content/Props/Crate.nmmesh" -- that is the file's
 * path under the project root, and also where the build (and StageAsset)
 * copies it beside the executable, which is where the running game reads it.
 * A material is named by its graph's base name ("Glow").
 */
namespace EditorContent
{
    /* InPath relative to the project root, forward slashes; empty if it is
     * outside the project. */
    QString ToRuntimePath(const QString& InPath);

    /* The material a file names: "Glow" for Glow.material.nmgraph,
     * Glow.material or Glow.fsh under a Materials folder. Empty otherwise. */
    QString MaterialNameFor(const QString& InPath);

    /* The one local file in a drag's data, or empty if it carries none or
     * several. */
    QString SingleDroppedFile(const QMimeData* InData);

    /* Copies a freshly written asset to its staged place beside the
     * executable and forgets any cached copy, so the viewport draws the new
     * one on its next frame. Nothing for a file outside the project. */
    void StageAsset(const QString& InPath);

    /* Removes InPath's staged copy, after the project file was deleted or
     * moved away. */
    void UnstageAsset(const QString& InPath);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONTENTPATHS_H
