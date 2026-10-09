/**
* VIBECODE CERTIFIED
*/
#ifndef NIMBLEEDITOR_EDITORICONS_H
#define NIMBLEEDITOR_EDITORICONS_H

#if WITH_EDITOR

#include <cstdint>

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
        Play,    // lucide "play"
        Pause,   // lucide "pause"
        Stop,    // lucide "square"
    };

    QIcon Get(EIcon InIcon);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORICONS_H
