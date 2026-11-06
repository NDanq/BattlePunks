#ifndef NIMBLEEDITOR_UIEDITORTAB_H
#define NIMBLEEDITOR_UIEDITORTAB_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QString>

class EditorContext;
class QAction;
class QComboBox;
class QLabel;
class UIDesignDocument;
class UIDesignerViewport;
class UIDetailsPanel;
class UIHierarchyPanel;
class UIPalettePanel;

/*
 * One UI document in the UI Designer: the canvas in the middle, the widget
 * palette and the hierarchy on the left, the details on the right. The
 * toolbar saves, undoes, picks the design resolution and zoom, snaps, and
 * switches to Preview -- the document taking input as in a game, its actions
 * logged. Saving stages the file, so Play-in-Editor picks it up at once.
 */
class UIEditorTab : public QMainWindow
{
    Q_OBJECT

public:
    explicit UIEditorTab(EditorContext& InContext, QWidget* InParent = nullptr);
    ~UIEditorTab() override;

    bool Load(const QString& InPath, QString* OutError = nullptr);

    const QString& GetPath() const;
    QString        GetTabTitle() const;
    bool           IsDirty() const;
    bool           Save();

    /* Asks to save if dirty. False if cancelled. */
    bool ConfirmClose();

    UIDesignDocument&   GetDesign() { return *Design; }
    UIDesignerViewport* GetViewport() { return Canvas; }

signals:
    void TitleChanged();
    void CloseRequested();

private:
    void BuildToolBar();
    void BuildDocks();
    void ApplyResolution(int InIndex);
    void AddFromPalette(const QString& InType);

    EditorContext&      Context;
    UIDesignDocument*   Design    = nullptr;
    UIDesignerViewport* Canvas    = nullptr;
    UIPalettePanel*     Palette   = nullptr;
    UIHierarchyPanel*   Hierarchy = nullptr;
    UIDetailsPanel*     Details   = nullptr;
    QComboBox*          Resolution = nullptr;
    QLabel*             ZoomLabel  = nullptr;
    QLabel*             Status     = nullptr;
    QAction*            PreviewAction = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIEDITORTAB_H
