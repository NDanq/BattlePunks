#ifndef NIMBLEEDITOR_PLUGINTEMPLATE_H
#define NIMBLEEDITOR_PLUGINTEMPLATE_H

#if WITH_EDITOR

#include <QString>
#include <QStringList>

/*
 * New Plugin: a plugin made from a starter in Engine/Templates/Plugin
 * (README.md there: Common/ plus a starter folder, __PLUGIN__ in names,
 * @PLUGIN_NAME@ and friends in contents, @IF_TEST@ / @IF_EDITOR@ /
 * @IF_CONTENT@ lines).
 *
 * Engine/cmake/NimblePluginTemplate.cmake (nimble_create_plugin, run by
 * Engine/Binaries/NewPlugin.cmake) does the same; NimbleEditorProjectTests
 * checks the two write the same files, byte for byte. Change both.
 */
namespace EditorPlugins
{
    struct NewPluginOptions
    {
        QString Name;
        QString Starter = QStringLiteral("Component");   // a folder of the template: Empty, Component, RenderPass
        QString Category;                                // empty: Gameplay
        QString Description;                             // one line; empty: "A NimbleEngine plugin"
        QString EngineVersion;                           // the descriptor's EngineVersion
        bool    bTest    = true;
        bool    bEditor  = false;
        bool    bContent = false;
    };

    /* Engine/Templates/Plugin of the engine this editor was built from; empty if missing. */
    QString GetPluginTemplateDir();

    /* The template's starters, the folders other than Common, sorted. */
    QStringList GetStarters(const QString& InTemplateDir);

    /* One line on what a starter makes, for the dialog. */
    QString DescribeStarter(const QString& InStarter);

    /* project(NimbleEngine VERSION x.y) from <InEngineDir>/CMakeLists.txt, as
     * the script reads it. Empty if not found. */
    QString ReadEngineVersion(const QString& InEngineDir);

    /* Names a new plugin cannot take: the engine's modules (Engine/Modules/*)
     * and its own targets. Add the plugins already found to it. */
    QStringList GetReservedNames(const QString& InEngineDir);

    /* Empty if InCategory can be one: letters, digits and spaces. */
    QString ValidateCategory(const QString& InCategory);

    /*
     * Makes InDestDir/<Name> from InTemplateDir. False, with OutError and
     * nothing left behind, if the name or category is invalid or taken (in
     * InReserved, without case), the starter missing, the folder already
     * there, or a file cannot be written. OutFiles: what was written,
     * relative to the plugin.
     */
    bool CreatePlugin(const QString& InTemplateDir, const QString& InDestDir, const NewPluginOptions& InOptions,
                      const QStringList& InReserved, QString& OutPluginDir, QString& OutError, QStringList* OutFiles = nullptr);

    /* [Plugins] <InName>=true|false in the config file at InPath (ConfigIniFile:
     * the rest of the file kept as it is). False, with OutError, if it cannot be written. */
    bool SetPluginEnabled(const QString& InConfigPath, const QString& InName, bool bInEnabled, QString& OutError);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PLUGINTEMPLATE_H
