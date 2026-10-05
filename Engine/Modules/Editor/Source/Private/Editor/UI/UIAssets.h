#ifndef NIMBLEEDITOR_UIASSETS_H
#define NIMBLEEDITOR_UIASSETS_H

#if WITH_EDITOR

#include <QString>

class UIEditorTab;
class QWidget;

/*
 * UI documents (.nmui, Engine/Modules/UI/README.md) as project files: where
 * new ones go, opening and creating them in the UI Designer, staging and
 * cooking. A document is named by runtime path ("Content/UI/HUD.nmui"), the
 * path GameSession::GetUI().Open takes, staged at the same path beside the exe.
 */
namespace EditorUI
{
    /* <project>/Content/UI. */
    QString GetDefaultFolder();

    /* Copies InPath beside the exe at its runtime path and drops the
     * runtime's cached copy (UI::UIAssetCache), so the next frame -- or
     * Play-in-Editor -- shows it. */
    void Stage(const QString& InPath);

    /* Adds or focuses its tab; nullptr (message box shown) if it cannot be opened. */
    UIEditorTab* OpenUI(const QString& InPath, QWidget* InParent);

    /* Asks for a name, writes a document with a Canvas root in InFolder
     * (GetDefaultFolder() if empty), and opens it. */
    UIEditorTab* CreateUI(const QString& InFolder, QWidget* InParent);

    /* Every .nmui under InRoot (the project) cooked to InOutRoot/<runtime path>.
     * How many were cooked; OutFailed counts the rest, OutErrors says why. */
    int CookAll(const QString& InRoot, const QString& InOutRoot, int& OutFailed, QString& OutErrors);

    /* Offers to save each unsaved document. False if cancelled. */
    bool CloseAll();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIASSETS_H
