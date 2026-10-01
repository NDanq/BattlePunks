#ifndef NIMBLEEDITOR_SETTINGSDIALOG_H
#define NIMBLEEDITOR_SETTINGSDIALOG_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QDialog>
#include <QString>

class QDialogButtonBox;

/*
 * Edit > Editor Settings: the [Editor] config vars as a paged form, per user.
 *
 * Rows read and write their var via ToString / SetFromString, like the .ini
 * and Console; the dialog stores nothing itself.
 *
 * Saved to EditorSettings.ini in AppConfigLocation, loaded after
 * DefaultEngine.ini (and before --rhi). Add a setting with one table row in
 * SettingsDialog.cpp.
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* InParent = nullptr);

    /* Where the user's settings live; the file need not exist. */
    static QString GetUserSettingsPath();

    /* Once, after Config/DefaultEngine.ini. */
    static void LoadUserSettings();

    /* As Apply does, for tools that set these vars (the Import Wizard). */
    static bool SaveUserSettings();

private:
    /* Type-agnostic: goes through ConfigVar::ToString / SetFromString. */
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
