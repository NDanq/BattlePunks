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
 * Edit > Project Settings: the project's Config/DefaultEngine.ini (shared),
 * as opposed to per-user Editor Settings.
 *
 * Edits the master copy in the game's source tree (EditorProject.h), then
 * refreshes the staged copy; without a game, the staged copy. Line-by-line
 * (ConfigIniFile), changed values only.
 *
 * Pages: Project (title, module, start level, startup script, and the
 * optional engine modules -- [Modules], build-time); one per
 * config section (registered vars, outside [Editor], with reset; keys set
 * in the file shown bold); and unregistered keys as text.
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
