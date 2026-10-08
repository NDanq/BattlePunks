#ifndef NIMBLEEDITOR_PROJECTBUILD_H
#define NIMBLEEDITOR_PROJECTBUILD_H

#if WITH_EDITOR

#include <QString>
#include <QStringList>

/*
 * Build > Build Project, without widgets: what a packaged build of the game
 * is, from the project's own CMakePresets.json. The game (Client target) is
 * configured and built in its own tree beside the project
 * (cmake-build-<preset>), never the editor's -- which this process holds.
 *
 * Options live in Config/DefaultEngine.ini's [Build], beside the world list
 * whose Cook flags say which worlds go in.
 */
namespace EditorProjectBuild
{
    enum EConfiguration { BC_Debug = 0, BC_RelWithDebInfo, BC_Shipping, BC_Count };

    /* "Debug", "RelWithDebInfo", "Shipping": CMAKE_BUILD_TYPE, and the
     * [Build] Configuration value. */
    const char*    GetConfigurationName(EConfiguration InConfiguration);
    EConfiguration ParseConfiguration(const QString& InText);   // Shipping if unknown

    /* What [Build] holds. */
    struct BuildOptions
    {
        EConfiguration Configuration = BC_Shipping;
        bool           bCook         = true;
        bool           bCompile      = true;
        bool           bOpenOutput   = true;
    };

    /* From / to [Build] in the project's config file (ProjectSettingsDialog's). */
    BuildOptions LoadOptions();
    bool         SaveOptions(const BuildOptions& InOptions, QString* OutError = nullptr);

    /*
     * The configure-and-build preset for a Client build in InConfiguration:
     * "client-<config>-clang" or "client-<config>", whichever the project's
     * CMakePresets.json has, is not hidden, and whose hostSystemName
     * condition holds here -- what `cmake --list-presets` would offer. Empty,
     * with OutError, if none.
     */
    QString FindClientPreset(const QString& InProjectRoot, EConfiguration InConfiguration, QString* OutError = nullptr);

    /* Where the game lands: <root>/Binaries/<Platform>_<Config>_Client
     * (Engine/CMakeLists.txt's NIMBLE_BINARIES_DIR layout). */
    QString GetOutputDir(const QString& InProjectRoot, EConfiguration InConfiguration);

    /* `cmake --preset <p>` and `cmake --build --preset <p>`. */
    QStringList ConfigureArgs(const QString& InPreset);
    QStringList BuildArgs(const QString& InPreset);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTBUILD_H
