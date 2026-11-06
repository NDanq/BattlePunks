#ifndef NIMBLEEDITOR_PROJECTTEMPLATE_H
#define NIMBLEEDITOR_PROJECTTEMPLATE_H

#if WITH_EDITOR

#include <QList>
#include <QString>
#include <QStringList>

/*
 * File > New Project, without widgets: a new game made from a template folder
 * (Engine/Templates/Game), into <parent>/<Name>. Never touches the engine --
 * the game configures standalone and adds the Engine tree itself.
 *
 * Every file of the template is copied, with these replaced in its contents
 * and "__PROJECT__" in its file name:
 *
 *   @PROJECT_NAME@   the name ("Asteroids"): targets, classes, the start level
 *   @ENGINE_DIR@     the Engine tree, as CMake reads it from the new project:
 *                    "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine", or absolute
 *                    where no relative path exists
 *
 * Engine/Binaries/NewProject.cmake does the same without the editor; change
 * both. NimbleEditorProjectTests checks they write identical projects.
 */
namespace EditorProject
{
    /* The template folder ("Engine/Templates/Game") of the engine this editor
     * was built from, else the one staged beside the executable; empty if
     * neither exists. */
    QString GetGameTemplateDir();

    /* The Engine tree, where Templates/ is; empty if unknown. */
    QString GetEngineDir();

    /* Where new games go: the Games folder beside the Engine tree. */
    QString GetDefaultGamesDir();

    /* Empty if InName can name a game -- a C++ identifier, it names classes
     * and targets -- else why not. */
    QString ValidateName(const QString& InName);

    /* @ENGINE_DIR@ for a project at InProjectDir. */
    QString EngineDirForCMake(const QString& InProjectDir, const QString& InEngineDir);

    /*
     * Copies InTemplateDir to InParentDir/InName. False, with OutError, if the
     * name is invalid, the folder already exists, or a file cannot be written;
     * a half-made project is removed. OutFiles: the files written, relative to
     * the project.
     */
    bool CreateProject(const QString& InTemplateDir, const QString& InEngineDir, const QString& InParentDir,
                       const QString& InName, QString& OutProjectDir, QString& OutError, QStringList* OutFiles = nullptr);

    /*
     * Example projects: whole games packed in Engine/Examples (pack_example.py),
     * listed in its Examples.json. Unpacked -- and renamed, configured, built
     * and cooked -- by Engine/Binaries/NewProject.cmake -DEXAMPLE=, which
     * File > New Project runs too, so the editor and the command line make
     * the same project (Engine/Examples/UnpackExample.cmake).
     */
    struct ExampleInfo
    {
        QString Name;            // "DemoLab": the folder and the project's own name
        QString Title;           // "Demo Lab"
        QString Description;
        QString EngineVersion;   // the engine it was packed against
        qint64  PackedBytes   = 0;
        qint64  UnpackedBytes = 0;
        int     Files         = 0;
    };

    /* Engine/Examples of the engine this editor was built from; empty if none. */
    QString GetExamplesDir();

    /* Engine/Binaries/NewProject.cmake; empty if missing. */
    QString GetNewProjectScript();

    /* The examples InExamplesDir's Examples.json lists, by name. Empty, with
     * OutError, if it cannot be read (none at all is not an error). */
    QList<ExampleInfo> ListExamples(const QString& InExamplesDir, QString* OutError = nullptr);

    /* The steps UnpackExample.cmake runs, in order. */
    QStringList GetExampleSteps(bool bInBuild);   // {"unpack"} or {"unpack","configure","build","cook"}

    /* `cmake <these>`: InSteps of making InParentDir/InName from InExample. */
    QStringList ExampleScriptArgs(const QString& InScript, const QString& InExamplesDir, const QString& InExample,
                                  const QString& InParentDir, const QString& InName, const QStringList& InSteps);

    /* "-- NewProject: [2/4] Configuring (...)" -> 2, 4, "Configuring (...)"; false for any other line. */
    bool ParseExampleStep(const QString& InLine, int& OutIndex, int& OutCount, QString& OutWhat);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTTEMPLATE_H
