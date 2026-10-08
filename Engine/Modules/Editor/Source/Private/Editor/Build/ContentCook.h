#ifndef NIMBLEEDITOR_CONTENTCOOK_H
#define NIMBLEEDITOR_CONTENTCOOK_H

#if WITH_EDITOR

#include <functional>

#include <QString>
#include <QStringList>

/*
 * Build > Cook Content, and Build Project's cook step: the project's content
 * in the binary form a Shipping build reads, mirrored under
 * <project>/Saved/Cooked/<source path> -- the build stages that tree beside
 * the executable, where the game reads each asset at its own path
 * (GameCore::ResolveContentWorld / ResolveContentPrefab).
 *
 * <project>/Saved/ is what the tools make, never source: Cooked/, and Logs/
 * -- the editor's session log, and one per cook and per Build Project run.
 * The Content Browser and every content walk skip it.
 *
 *   Graphs    every node graph regenerated (EditorGraphs::RegenerateAll): the
 *             build cooks material graphs' saved code into shader caches
 *   Worlds    the [World_*] list's worlds with Cook=true; an excluded world's
 *             cooked copy is deleted, so it cannot be staged
 *   Prefabs   every .nmprefab: game code spawns them by path, so a world's
 *             own placements do not say which are needed
 *   UI        every .nmui, for the same reason
 *
 * Synchronous, on the main thread; InProgress hears each step.
 */
namespace EditorCook
{
    enum EAssetType { AT_Graphs = 0, AT_Worlds, AT_Prefabs, AT_UI, AT_Count };

    const char* GetTypeName(EAssetType InType);

    struct TypeStats
    {
        int    Cooked   = 0;
        int    Failed   = 0;
        int    Removed  = 0;   // stale cooked copies deleted (excluded worlds)
        qint64 BytesIn  = 0;   // the sources (a world: its manifest and entity folder)
        qint64 BytesOut = 0;   // what was written
        double Seconds  = 0.0;
    };

    struct CookStats
    {
        TypeStats   Types[AT_Count];
        QStringList Failures;      // "<path>: <why>"
        QStringList Worlds;        // the worlds cooked, by name
        QStringList Excluded;      // listed with Cook=false
        double      Seconds = 0.0;

        int  GetFailedCount() const;
        bool Succeeded() const { return GetFailedCount() == 0; }
    };

    /* <InProjectRoot>/Saved: what the tools make. */
    QString GetSavedRoot(const QString& InProjectRoot);

    /* <InProjectRoot>/Saved/Cooked: where every cooked file goes. */
    QString GetCookedRoot(const QString& InProjectRoot);

    /* <InProjectRoot>/Saved/Logs: a log per run. */
    QString GetLogsDir(const QString& InProjectRoot);

    /* A new run log's path, "<Logs>/<InKind>-YYYY.MM.DD-HH.MM.SS.log", its
     * folder made, keeping only the newest InKeep - 1 older "<InKind>-*.log"
     * (as EngineCore::OpenRunLog). */
    QString NewLogPath(const QString& InProjectRoot, const QString& InKind, int InKeep = 10);

    /* "2026-10-07 18:16:28.121 ": what starts each line of a run log, as
     * EngineCore's run logs do. */
    QString LogStamp();

    /* A run log's first line: "<InKind> log, started <date> <time>". */
    QString LogHeader(const QString& InKind);

    /* The cooked path for a runtime path ("Content/Levels/A.nmworld"). */
    QString GetCookedPath(const QString& InProjectRoot, const QString& InRuntimePath);

    /* InProjectRoot's content, cooked. InProjectRoot empty: the editor's project
     * (EditorGraphs::GetProjectRoot). */
    CookStats CookProject(const QString& InProjectRoot = QString(),
                          const std::function<void(const QString&)>& InProgress = {});

    /* A table of InStats, one line per type and a total, for the Console. */
    QStringList DescribeStats(const CookStats& InStats);

    /* "1.2 MB". */
    QString FormatBytes(qint64 InBytes);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONTENTCOOK_H
