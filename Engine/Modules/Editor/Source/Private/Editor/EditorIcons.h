/**
* VIBECODE CERTIFIED
*/
#ifndef NIMBLEEDITOR_EDITORICONS_H
#define NIMBLEEDITOR_EDITORICONS_H

#if WITH_EDITOR

#include <cstdint>

#include <QColor>
#include <QIcon>

/*
 * The editor's icons: Lucide (ISC), vendored at External/lucide-1.53.0 and
 * compiled into the executable as Qt resources (:/lucide/<name>.svg).
 *
 * Lucide draws in "currentColor"; these icons draw it in the application
 * palette's ButtonText (its Disabled one when disabled), read when painted,
 * so they follow Editor.Theme without being rebuilt. Sharp at any size and
 * device pixel ratio.
 *
 * Without Qt Svg (the build finds it optionally), or a resource missing, the
 * QStyle's standard icon for the same thing.
 *
 * To add one: copy its .svg from lucide-static into External/lucide-<ver>/icons,
 * list it in the CMakeLists' qt_add_resources, and add it to EIcon and the
 * table in EditorIcons.cpp.
 */
namespace EditorIcons
{
    enum class EIcon : uint8_t
    {
        Play,         // lucide "play"
        Pause,        // lucide "pause"
        Stop,         // lucide "square"
        Undo,         // lucide "undo-2"
        Redo,         // lucide "redo-2"
        Save,         // lucide "save"
        SkipBack,     // lucide "skip-back": to the start
        SkipForward,  // lucide "skip-forward": to the end
        StepBack,     // lucide "step-back": a frame back
        StepForward,  // lucide "step-forward": a frame on
        Loop,         // lucide "repeat"
        Record,       // lucide "circle-dot": auto-key
        Key,          // lucide "key-round": set a key
        CameraCuts,   // lucide "clapperboard": look through the shot
        Pilot,        // lucide "plane": fly the shot camera
        Snap,         // lucide "magnet"
        Add,          // lucide "plus"
        ZoomIn,       // lucide "zoom-in"
        ZoomOut,      // lucide "zoom-out"
        Fit,          // lucide "scan": fit the view
        Sound,        // lucide "volume-2": hear the preview
        Scrub,        // lucide "audio-waveform": hear the scrub
        Subtitles,    // lucide "captions"
        Eye,          // lucide "eye": show an asset field's asset in the Content Browser
        Help,         // lucide "circle-question-mark": a component's documentation
        Terrain,            // the editor's own (Resources/Icons), drawn to Lucide's grid: a mountain
        TerrainRaise,       // a hump and an arrow up
        TerrainLower,       // a dip and an arrow down
        TerrainSmooth,      // waves easing out
        TerrainFlatten,     // an arrow down onto a level line
        TerrainNoise,       // a jagged line
        TerrainSetHeight,   // a height gauge
        TerrainPaint,       // a brush
        TerrainSmoothPaint, // two overlapping circles
        Foliage,            // a tree
        FoliageErase,       // a small tree and a cross
        Bell,         // lucide "bell": notifications
        BellOff,      // lucide "bell-off": do not disturb
        Error,        // lucide "circle-x"
        Success,      // lucide "circle-check"
        Warning,      // lucide "triangle-alert"
        Info,         // lucide "info"
        Close,        // lucide "x"
        Branch,       // lucide "git-branch"
        Refresh,      // lucide "refresh-cw"
        Ahead,        // lucide "arrow-up": commits to push
        Behind,       // lucide "arrow-down": commits to pull
        Fetch,        // lucide "cloud-download"
        Push,         // lucide "upload"
        Commit,       // lucide "git-commit-horizontal"
        Lock,         // lucide "lock": an LFS lock
        Merge,        // lucide "git-merge": a conflict
        Back,         // lucide "arrow-left"
        Forward,      // lucide "arrow-right": also "go to" a linked thing
        FolderUp,     // lucide "folder-up"
        NewFolder,    // lucide "folder-plus"
        Browse,       // lucide "folder-open": pick a file or asset
        Folder,       // lucide "folder"
        Document,     // lucide "file-text"
        ListView,     // lucide "list"
        Import,       // lucide "import"
        External,     // lucide "external-link": open outside the editor
        Reset,        // lucide "rotate-ccw": back to the default, or start again
        Remove,       // lucide "minus"
        MoveUp,       // lucide "chevron-up": earlier in a list
        MoveDown,     // lucide "chevron-down": later in a list
        Clear,        // lucide "brush-cleaning": empty a log
        Count
    };

    QIcon Get(EIcon InIcon);

    /* Drawn in InColor rather than the palette's (the notification severities);
     * disabled, still in the palette's. */
    QIcon Get(EIcon InIcon, const QColor& InColor);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORICONS_H
