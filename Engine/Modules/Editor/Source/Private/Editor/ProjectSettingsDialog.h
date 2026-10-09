#ifndef NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H
#define NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QDialog>
#include <QString>

#include "Private/Editor/ConfigIniFile.h"

class InputMapsPage;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QStackedWidget;
class QTableWidget;
class QWidget;

namespace EngineCore { class ConfigVar; }

/*
 * Edit > Project Settings: the project's Config/DefaultEngine.ini (shared),
 * as opposed to per-user Editor Settings.
 *
 * Edits the master copy in the game's source tree (EditorProject.h), then
 * refreshes the staged copy; without a game, the staged copy. Line-by-line
 * (ConfigIniFile), changed values only.
 *
 * Pages: Project (title, module, start world, and the
 * optional engine modules -- [Modules], build-time); Worlds (the project's
 * [World_*] list, NimbleEngine/WorldList.h: every world a level, with its
 * game mode, startup script and whether it is cooked); Input (the game's
 * input maps, [Input_*]: InputMapsPage.h); one per config
 * section (registered vars, outside [Editor] and [Build], with reset; keys
 * set in the file shown bold); and unregistered keys as text.
 *
 * Apply also sets the vars in-process; most take effect at startup or Play.
 */
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(QWidget* InParent = nullptr);

    /* The master copy if known, else the staged one (OutbIsSource false). */
    static QString GetProjectConfigPath(bool* OutbIsSource = nullptr);

    /* Shows the Worlds page. */
    void ShowWorlds();

    /* The world open in the editor (runtime path): Worlds > Add Open World. */
    void SetOpenWorld(const QString& InRuntimePath) { OpenWorld = InRuntimePath; }

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
    void     BuildWorldsPage();
    void     BuildSectionPages();
    void     BuildInputPage();

    /* One row of the Worlds table, from a [World_*] section. */
    void     AddWorldRow(const ConfigIniFile::Section& InSection);
    /* A new row for a .nmworld (runtime path), named for it. */
    void     AddWorld(const QString& InRuntimePath);
    /* The table as [World_*] sections, in its order. */
    QList<ConfigIniFile::Section> ReadWorlds() const;
    /* Empty if the table can be saved, else why not. */
    QString  ValidateWorlds() const;
    /* [Game] StartLevel's choices: the table's names. */
    void     RefreshStartLevelChoices();
    QWidget* AddPage(const QString& InName, QFormLayout*& OutForm);
    void     AddRow(QFormLayout* InForm, const QString& InSection, const QString& InKey, const QString& InLabel);
    /* False if the dialog should stay open: a module change not confirmed,
     * or the file could not be written. */
    bool     Apply();
    /* Optional module switches changed: asks whether to save them, as they
     * need a native recompile. False (and the switches put back) if not. */
    bool     ConfirmModuleChanges();
    void     UpdateLabel(const Row& InRow);

    ConfigIniFile     File;
    QString           FilePath;
    bool              bFileIsSource = false;

    std::vector<Row>  Rows;
    QTableWidget*     Worlds = nullptr;
    InputMapsPage*    Input = nullptr;
    QList<ConfigIniFile::Section> WorldsBaseline;
    QComboBox*        StartLevelChoice = nullptr;
    QString           OpenWorld;
    QListWidget*      Pages = nullptr;
    QStackedWidget*   Stack = nullptr;
    QDialogButtonBox* Buttons = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTSETTINGSDIALOG_H
