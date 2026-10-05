#ifndef NIMBLEEDITOR_UIDESIGNDOCUMENT_H
#define NIMBLEEDITOR_UIDESIGNDOCUMENT_H

#if WITH_EDITOR

#include <functional>
#include <memory>

#include <QObject>
#include <QString>

#include "Public/UI/UIDocument.h"

class QUndoStack;

/*
 * One .nmui as the UI Designer edits it: the document (UI::UIDocumentAsset,
 * exactly what the file holds), its undo history and the selected widget.
 *
 * Every change goes through Edit, which records the whole document before
 * and after -- documents are small, and a snapshot can never get out of step
 * with what an edit did, the GraphDocument trade. Edits with the same merge
 * key in a row become one undo step: a drag, or a value typed twice.
 *
 * Widgets are named by index into the asset, which is also the index of
 * each widget in an instance of it (UI::UIDocumentInstance).
 */
class UIDesignDocument : public QObject
{
    Q_OBJECT

public:
    explicit UIDesignDocument(QObject* InParent = nullptr);
    ~UIDesignDocument() override;

    /* Reads InPath (XML). False with OutError, the document unchanged. */
    bool Load(const QString& InPath, QString* OutError = nullptr);

    /* Writes Path (XML, deterministic), stages it beside the exe and drops
     * the runtime's cached copy, so Play-in-Editor shows it. */
    bool Save(QString* OutError = nullptr);

    /* A document that is not on disk yet, or one being saved elsewhere. */
    void           SetPath(const QString& InPath) { Path = InPath; }
    const QString& GetPath() const { return Path; }

    const UI::UIDocumentAsset& GetAsset() const { return Asset; }
    /* A shared copy for an instance (the viewport's preview); remade after each edit. */
    std::shared_ptr<const UI::UIDocumentAsset> GetSharedAsset() const { return Shared; }

    /*
     * Changes the document through InEdit, as one undoable step named
     * InLabel. A non-empty InMergeKey joins it to the step before it if that
     * one had the same key. An edit that changes nothing records nothing.
     * bInStructure says widgets were added, removed or moved (the hierarchy
     * rebuilds); otherwise only values changed.
     */
    void Edit(const QString& InLabel, const std::function<void(UI::UIDocumentAsset&)>& InEdit,
              const QString& InMergeKey = QString(), bool bInStructure = false);

    /* InWidget's property InName set to InValue as text, or removed when
     * InValue is null (back to its style's or its type's): one Edit. */
    void SetProperty(int InWidget, const QString& InName, const QString& InValue);
    void ResetProperty(int InWidget, const QString& InName);

    /* A widget of InType added to InParent (before its child InBefore, or
     * last), with InProperties; selected. Its index, or -1 if refused. */
    int AddWidget(const QString& InType, int InParent, int InBefore, const std::vector<UI::UIPropertyText>& InProperties = {});

    /* The selected widget's subtree gone (not the root); the parent selected. */
    void DeleteSelected();

    /* The selected widget's subtree copied after it; the copy selected. */
    void DuplicateSelected();

    /* InIndex's subtree to InNewParent before InBefore (-1 last); selected. */
    bool MoveWidget(int InIndex, int InNewParent, int InBefore);

    /* As XML (UI::WriteUIXmlFragment), and back in as InParent's last child (or the selection's). */
    QString CopySelected() const;
    bool    Paste(const QString& InXml);

    // === Selection =========================================================

    /* -1: nothing (the document's own settings). */
    int  GetSelected() const { return Selected; }
    void Select(int InWidget);

    QUndoStack* GetUndoStack() const { return Undo; }
    bool        IsDirty() const;
    void        MarkClean();

signals:
    /* The asset changed (an edit, an undo or a load). bInStructure: widgets added, removed or moved. */
    void Changed(bool bInStructure);
    void SelectionChanged(int InWidget);
    void DirtyChanged(bool bInDirty);

    /* An edit (not an undo or redo) renamed a widget, or removed some: their
     * UI graph handlers may want following (UIGraphLink.h). After Changed. */
    void WidgetRenamed(const QString& InOld, const QString& InNew);
    void WidgetsRemoved(const QStringList& InNames);

private:
    friend class UISnapshotCommand;

    /* Undo and redo's: the asset as it was, without recording. */
    void Restore(const UI::UIDocumentAsset& InAsset, int InSelected, bool bInStructure);
    void Publish(bool bInStructure);

    UI::UIDocumentAsset                         Asset;
    std::shared_ptr<const UI::UIDocumentAsset>  Shared;
    QString                                     Path;
    QUndoStack*                                 Undo = nullptr;
    int                                         Selected = -1;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIDESIGNDOCUMENT_H
