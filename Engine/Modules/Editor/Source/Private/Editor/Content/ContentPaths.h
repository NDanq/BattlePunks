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

    /* Every material graph's runtime path, sorted: the project's and the
     * engine's ("Materials/Glow.material.nmgraph", "Engine/Content/Materials/..."). */
    QStringList ListMaterialPaths();

    /* What a material field stores for a file: the runtime path of a
     * *.material.nmgraph in the project or the engine's content. Empty for
     * anything else -- material fields take material graphs only. */
    QString MaterialPathFor(const QString& InPath);

    /* Whether InText is what a material field may hold: "" or a runtime path
     * ending .material.nmgraph. */
    bool IsMaterialFieldValue(const QString& InText);

    /* A post process field (PF_AssetPostProcess): a stack, *.pp.nmgraph -- or
     * a PostProcess material graph, a stack of one. Every stack's runtime path,
     * sorted; what the field stores for a file ("" for anything else); and
     * whether text is a value it may hold. */
    QStringList ListPostProcessPaths();
    QString     PostProcessPathFor(const QString& InPath);
    bool        IsPostProcessFieldValue(const QString& InText);

    /* A render target field (PF_AssetRenderTarget): an .nmrt2d or .nmrt3d
     * (RenderTargetAsset.h), or a transient "transient://Name". Every one's
     * runtime path in the project, sorted; what the field stores for a file
     * ("" for anything else); and whether text is a value it may hold. */
    QStringList ListRenderTargetPaths();

    /* Every .nmtexture in the project and the engine, by runtime path; and one file's (empty if not a texture). */
    QStringList ListTexturePaths();

    /* A particle system field (PF_AssetParticleSystem): every .nmparticle in
     * the project and the engine, by runtime path; and one file's (empty if
     * not a particle system). */
    QStringList ListParticleSystemPaths();
    QString     ParticleSystemPathFor(const QString& InPath);
    /* Sequences (*.nmseq): every one in the project, and a file's runtime path. */
    QStringList ListSequencePaths();
    QString     SequencePathFor(const QString& InPath);

    /* An animation field (PF_AssetAnimation): an .nmanim clip or an
     * .nmanimstate anim state. Every one's runtime path in the project and
     * the engine, sorted; and one file's (empty if neither). */
    QStringList ListAnimationPaths();
    QString     AnimationPathFor(const QString& InPath);

    /* A sound field (PF_AssetSound): every .nmsound in the project and the
     * engine, by runtime path; and one file's (empty if not a sound). */
    QStringList ListSoundPaths();
    QString     SoundPathFor(const QString& InPath);

    /* A prefab field (PF_AssetPrefab): every .nmprefab in the project and the
     * engine, by runtime path, then every registered native prefab
     * ("native:Name"); and one file's (empty if not a prefab). */
    QStringList ListPrefabPaths();
    QString     PrefabPathFor(const QString& InPath);
    QString     TexturePathFor(const QString& InPath);
    QString     RenderTargetPathFor(const QString& InPath);
    bool        IsRenderTargetFieldValue(const QString& InText);

    /* What a Script field stores for a file: the runtime path of a .lua or a
     * *.script.nmgraph in the project. Empty for anything else. */
    QString ScriptPathFor(const QString& InPath);

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
