#ifndef NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H
#define NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QDialog>
#include <QString>

#include "Private/Editor/ConfigIniFile.h"

class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QStackedWidget;
class QWidget;

namespace EngineCore { class ConfigVar; }

/*
 * Edit > Project Settings: the project's Config/DefaultEngine.ini as pages of
 * settings -- what the game and the engine read, as opposed to Editor
 * Settings, which are one person's and live in their profile.
 *
 * WHAT IT EDITS. The project's master copy of the file, the one in the game's
 * source tree (Games/Sandbox/Config/DefaultEngine.ini), whose path the game's
 * CMakeLists hands the editor at configure time (EditorProject.h). Not the
 * copy staged beside the executable: a build overwrites that. After a save
 * the staged copy is refreshed too, so the next Play reads the new values
 * without a build. An editor with no game configured edits the staged copy,
 * and says so.
 *
 * Edits are line by line (ConfigIniFile): every comment stays where it was.
 * Only changed values are written. A key the file does not have is added to
 * the end of its section.
 *
 * WHAT IT SHOWS.
 *  - Project: the few settings that say what the project is -- title, game
 *    module, start level, startup script -- and where things are.
 *  - One page per config section: every registered NIMBLE_CONFIG_* var in it
 *    (outside [Editor], which is Editor Settings'), with a widget for its type
 *    and a reset to the compiled default. A key the file sets bold.
 *  - Keys in the file that no loaded code registers -- a game module's own,
 *    when the module is not loaded (it is only loaded while playing) -- as
 *    text, so they can still be read and changed.
 *
 * Apply also sets the vars in this process, as the Console would. Most are
 * read at startup or at Play, so they take effect then.
 *
 * Private to the editor: EditorMainWindow opens it.
 */
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(QWidget* InParent = nullptr);

    /* The file Apply writes: the project's master copy when known, else the
     * staged one. OutbIsSource false for the staged fallback. */
    static QString GetProjectConfigPath(bool* OutbIsSource = nullptr);

private:
    struct Row
    {
        QString                             Section;
        QString                             Key;
        QString                             Baseline;   // what the file says (or the default), as shown
        bool                                bInFile = false;
        QLabel*                             Label = nullptr;
        std::function<void(const QString&)> Show;
        std::function<QString()>            Read;
    };

    void     BuildProjectPage();
    void     BuildSectionPages();
    QWidget* AddPage(const QString& InName, QFormLayout*& OutForm);
    void     AddRow(QFormLayout* InForm, const QString& InSection, const QString& InKey, const QString& InLabel);
    void     Apply();
    void     UpdateLabel(const Row& InRow);

    ConfigIniFile     File;
    QString           FilePath;
    bool              bFileIsSource = false;

    std::vector<Row>  Rows;
    QListWidget*      Pages = nullptr;
    QStackedWidget*   Stack = nullptr;
    QDialogButtonBox* Buttons = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H
