#ifndef NIMBLEENGINE_SHADERCOOK_H
#define NIMBLEENGINE_SHADERCOOK_H

#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace EngineCore { class LayeredFileProvider; }

/*
 * A development build's shaders, straight from NimbleCook's output
 * (Engine/Tools/NimbleCook) rather than from what the build last staged.
 *
 * NimbleCook cooks into <root>/Saved/Cooked/Shaders/<Variant>/, laid out as
 * the runtime reads it, for the engine (<root> the Engine folder) and a
 * project; the build copies those folders beside the executable
 * (Engine/CMakeLists.txt, "Cooked shaders"). A Debug or RelWithDebInfo build
 * also reads them where they are:
 *
 *   the game     mounts each Runtime/ over the staged files
 *                (Application::Initialize), so a cook since the last build is
 *                seen without one; and runs NimbleCook itself, beside the
 *                executable, when the engine's has never been cooked
 *   the editor   runs NimbleCook at startup, before its renderer, and copies
 *                what is newer beside itself (RestageCookedShaders): its own
 *                on-save cooks write there too, and win when newer
 *
 * Shipping has none of it: it reads only what was staged or packed.
 */
namespace NimbleEngine::ShaderCook
{
    /* The Engine folder this build was made from; empty in Shipping, whose
     * binaries name no source path. */
    NE_API std::string GetEngineSourceDir();

    /* <InRoot>/Saved/Cooked/Shaders. */
    NE_API std::string GetCookedShaderDir(const std::string& InRoot);

    /* Whether NimbleCook has cooked InVariant ("Runtime", "Editor") under InRoot. */
    NE_API bool IsCooked(const std::string& InRoot, const char* InVariant);

    /* NimbleCook in InDir (the executable's folder), or empty. */
    NE_API std::string FindCooker(const std::string& InDir);

    /* `NimbleCook shaders -engine <InEngineDir> [-project <InProjectDir>]`,
     * waited for, each line it prints logged. True if it exited 0. For a
     * console program: a GUI process (the editor) runs it with QProcess. */
    NE_API bool RunCooker(const std::string& InCooker, const std::string& InEngineDir, const std::string& InProjectDir);

    /* The engine's and InProjectDir's (if any) cooked Runtime/ mounted as
     * layers "CookedShaders.Engine" and "CookedShaders.Project", over what is
     * already there. How many were mounted. */
    NE_API int MountCookedShaders(EngineCore::LayeredFileProvider& InAssets, const std::string& InEngineDir,
                                  const std::string& InProjectDir);

    /* Every file of the engine's and InProjectDir's cooked Runtime/ (and, with
     * bInEditor, Editor/) copied into InRuntimeDir where it is newer than the
     * copy there, or missing. How many files were copied. */
    NE_API int RestageCookedShaders(const std::string& InRuntimeDir, const std::string& InEngineDir,
                                    const std::string& InProjectDir, bool bInEditor);
}

#endif //NIMBLEENGINE_SHADERCOOK_H
