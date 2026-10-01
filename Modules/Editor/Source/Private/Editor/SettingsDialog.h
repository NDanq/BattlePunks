#ifndef NIMBLEEDITOR_SETTINGSDIALOG_H
#define NIMBLEEDITOR_SETTINGSDIALOG_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QDialog>
#include <QString>

class QDialogButtonBox;

/*
 * Edit > Editor Settings: the editor's config vars ([Editor] in the config
 * grammar) as a form, grouped by page, saved per user.
 *
 * THE VARS ARE THE SETTINGS. The dialog holds no values of its own: each row
 * names a NIMBLE_CONFIG_* var (Editor.SnapTranslate, ...), reads it to fill
 * its widget, and Apply / OK write it back with SetFromString -- the same
 * entry point the .ini and the Console use, so all three agree on what a
 * value means. A var set from the Console shows its new value the next time
 * the dialog opens.
 *
 * THE USER FILE. Apply / OK also write every row's var to EditorSettings.ini
 * in the user's config directory (QStandardPaths::AppConfigLocation), in the
 * config grammar. main() loads it after Config/DefaultEngine.ini, so a user's
 * choice beats the project's, and --rhi beats both. It is not beside the
 * executable: Binaries/ is per configuration, and settings are per person.
 *
 * Adding a setting is one row in SettingsDialog.cpp's table.
 *
 * Private to the editor: EditorMainWindow opens it.
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* InParent = nullptr);

    /* Where the user's settings live; the file need not exist. */
    static QString GetUserSettingsPath();

    /* Loads the user file over whatever is set, if it exists. Call once,
     * after Config/DefaultEngine.ini. */
    static void LoadUserSettings();

    /* Writes every setting's current value to the user file, as Apply does --
     * for a tool that sets one of these vars itself (the Import Wizard). */
    static bool SaveUserSettings();

private:
    /* One var's widget. The text is the var's own (ConfigVar::ToString /
     * SetFromString), so a row needs no knowledge of the var's type. */
    struct Row
    {
        const char*                         Path = nullptr;   // "Editor.SnapTranslate"
        std::function<void(const QString&)> Show;             // text -> widget
        std::function<QString()>            Read;             // widget -> text
        bool                                bRestart = false;
    };

    void Apply();
    bool Save() const;
    void RestoreDefaults();

    std::vector<Row>  Rows;
    QDialogButtonBox* Buttons = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SETTINGSDIALOG_H
