#ifndef NIMBLEEDITOR_EDITORTHEME_H
#define NIMBLEEDITOR_EDITORTHEME_H

#if WITH_EDITOR

/*
 * Editor.Theme: 0 = system, 1 = light, 2 = dark; applies immediately.
 *
 * Always Fusion with explicit palettes: the native Windows style ignores the
 * palette and has no dark mode. Qt's colour scheme is set too, for the title
 * bar.
 */
namespace EditorTheme
{
    /* Once, after config load and before any window is shown. */
    void Install();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORTHEME_H
