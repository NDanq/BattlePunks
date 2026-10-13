#ifndef NIMBLEEDITOR_STARTUPCOOK_H
#define NIMBLEEDITOR_STARTUPCOOK_H

#if WITH_EDITOR

#include "Public/RenderCore/RenderTypes.h"

/*
 * The editor's startup shader cook, behind a splash screen: NimbleCook beside
 * the editor (EditorCook::GetCookerPath) for the engine and the project, then
 * what is newer copied beside the editor (NimbleEngine/ShaderCook.h), all
 * before the main window -- and its renderer -- exist. Up to date it takes a
 * moment; a fresh checkout's first run, minutes, which the splash shows
 * NimbleCook's progress through.
 *
 * Skipped for the Null backend, which loads no shaders, with [Editor]
 * CookShadersOnStartup off, or without NimbleCook. A failed cook is logged
 * and the editor starts on what was cooked before. Blocks the caller in a
 * local event loop, so call it once config is loaded and before the window.
 */
namespace EditorStartupCook
{
    void Run(RenderCore::ERenderBackend InBackend);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_STARTUPCOOK_H
