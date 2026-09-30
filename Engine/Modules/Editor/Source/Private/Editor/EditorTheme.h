#ifndef NIMBLEEDITOR_EDITORTHEME_H
#define NIMBLEEDITOR_EDITORTHEME_H

#if WITH_EDITOR

/*
 * Light or dark UI: Editor.Theme, 0 = follow the system, 1 = light, 2 = dark
 * (Edit > Editor Settings > Appearance, or `Editor.Theme 2` in the Console).
 * A change applies at once, to every open window.
 *
 * WHY FUSION, ALWAYS. Qt's native Windows 10 style ("windowsvista") draws
 * with the system's light theme parts whatever the palette says, so it has no
 * dark mode. Fusion draws everything from the palette, so the editor uses it
 * in every mode -- one look, light or dark, rather than native when light and
 * Fusion when dark. The palettes are set here explicitly rather than taken
 * from the platform theme, so both modes look the same on every Windows.
 *
 * Qt's requested colour scheme is set too, for what Qt draws outside the
 * palette (the window title bar, where the platform supports it).
 *
 * Private to the editor.
 */
namespace EditorTheme
{
    /* Applies Editor.Theme now, and keeps "follow the system" following it.
     * Call once, after the config is loaded and before any window is shown.
     * The var's callback applies later changes. */
    void Install();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORTHEME_H
