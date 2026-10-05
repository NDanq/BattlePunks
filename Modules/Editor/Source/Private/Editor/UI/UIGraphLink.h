#ifndef NIMBLEEDITOR_UILOGICLINK_H
#define NIMBLEEDITOR_UILOGICLINK_H

#if WITH_EDITOR

#include <QObject>
#include <QString>
#include <QStringList>

class QWidget;
class UIDesignDocument;

/*
 * The UI Designer's link to a document's UI graph (its Graph setting, a
 * *.ui.nmgraph -- UI/README.md, "UI graphs"): making one beside the
 * document, and making or finding the UI Event a widget's action calls,
 * opened in the Graph Editor on that node.
 *
 * A handler is named <Widget>_<Event> ("Resume_OnClick", "Document_OnBack"
 * for the document's own) and called by the action Graph:<Handler>; renaming
 * the widget later breaks nothing.
 */
/* Says when a UI graph was saved (EditorGraphs::StageOutput), naming the
 * document it is the graph of: the designer's Preview reloads it. */
class UIGraphNotifier : public QObject
{
    Q_OBJECT

signals:
    void GraphSaved(const QString& InDocumentRuntimePath, const QString& InGraphRuntimePath);
};

namespace EditorUIGraph
{
    /* The one notifier. */
    UIGraphNotifier& Notifier();

    /* The graph file InDesign's Graph setting names, on disk; empty if none is set. */
    QString GetGraphPath(const UIDesignDocument& InDesign);

    /* Its UI graph, made beside it (Content/UI/Menu.ui.nmgraph) and set as
     * its Graph (one undo step) if it has none -- asking first if bInAsk.
     * The graph file; empty if declined or it could not be written. */
    QString EnsureGraph(UIDesignDocument& InDesign, QWidget* InParent, bool bInAsk);

    /* The handler for InProperty (an Action) of widget InWidget, or of the
     * document with -1: found in the graph or added to it (saved, if the
     * graph had no other unsaved edits), the Graph Editor opened on it, and
     * Graph:<Handler> added to the property. Its name; empty if not done. */
    QString AddHandler(UIDesignDocument& InDesign, int InWidget, const QString& InProperty, QWidget* InParent);

    /* Opens the UI graph on InHandler's UI Event. False if there is none. */
    bool RevealHandler(const UIDesignDocument& InDesign, const QString& InHandler, QWidget* InParent);

    /* The UI Event handler names in the UI graph -- the open tab's if it is
     * open, else the file's. */
    QStringList GetHandlers(const UIDesignDocument& InDesign);

    /* The Graph:<Handler> names in an action text. */
    QStringList FindGraphActions(const QString& InActions);

    /* The handler name for InProperty of a widget named InWidget ("" for the document). */
    QString MakeHandlerName(const QString& InWidget, const QString& InProperty);

    /* The UI Events made for a widget named InWidget (their Widget note). */
    QStringList HandlersForWidget(const UIDesignDocument& InDesign, const QString& InWidget);

    /*
     * A widget was renamed InOld -> InNew: its UI Events' Widget note follows,
     * and with bInRenameHandlers a handler still named for it (Old_OnClick)
     * becomes New_OnClick, with the document's Graph:Old_OnClick actions --
     * one graph edit (saved if the graph had no other edits) and one document
     * edit. The handlers renamed.
     */
    QStringList RenameWidgetHandlers(UIDesignDocument& InDesign, const QString& InOld, const QString& InNew,
                                     bool bInRenameHandlers);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UILOGICLINK_H
