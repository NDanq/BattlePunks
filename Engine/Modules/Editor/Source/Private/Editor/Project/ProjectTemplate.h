#ifndef NIMBLEEDITOR_PROJECTTEMPLATE_H
#define NIMBLEEDITOR_PROJECTTEMPLATE_H

#if WITH_EDITOR

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
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTTEMPLATE_H
