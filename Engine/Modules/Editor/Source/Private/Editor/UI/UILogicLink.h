#ifndef NIMBLEEDITOR_UILOGICLINK_H
#define NIMBLEEDITOR_UILOGICLINK_H

#if WITH_EDITOR

#include <QString>
#include <QStringList>

class QWidget;
class UIDesignDocument;

/*
 * The UI Designer's link to a document's logic graph (its Logic setting, a
 * *.ui.nmgraph -- UI/README.md, "Logic graphs"): making one beside the
 * document, and making or finding the UI Event a widget's action calls,
 * opened in the Graph Editor on that node.
 *
 * A handler is named <Widget>_<Event> ("Resume_OnClick", "Document_OnBack"
 * for the document's own) and called by the action Graph:<Handler>; renaming
 * the widget later breaks nothing.
 */
namespace EditorUILogic
{
    /* The graph file InDesign's Logic names, on disk; empty if none is set. */
    QString GetGraphPath(const UIDesignDocument& InDesign);

    /* Its logic graph, made beside it (Content/UI/Menu.ui.nmgraph) and set as
     * its Logic (one undo step) if it has none -- asking first if bInAsk.
     * The graph file; empty if declined or it could not be written. */
    QString EnsureGraph(UIDesignDocument& InDesign, QWidget* InParent, bool bInAsk);

    /* The handler for InProperty (an Action) of widget InWidget, or of the
     * document with -1: found in the graph or added to it (saved, if the
     * graph had no other unsaved edits), the Graph Editor opened on it, and
     * Graph:<Handler> added to the property. Its name; empty if not done. */
    QString AddHandler(UIDesignDocument& InDesign, int InWidget, const QString& InProperty, QWidget* InParent);

    /* Opens the logic graph on InHandler's UI Event. False if there is none. */
    bool RevealHandler(const UIDesignDocument& InDesign, const QString& InHandler, QWidget* InParent);

    /* The UI Event handler names in the logic graph -- the open tab's if it is
     * open, else the file's. */
    QStringList GetHandlers(const UIDesignDocument& InDesign);

    /* The Graph:<Handler> names in an action text. */
    QStringList FindGraphActions(const QString& InActions);

    /* The handler name for InProperty of a widget named InWidget ("" for the document). */
    QString MakeHandlerName(const QString& InWidget, const QString& InProperty);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UILOGICLINK_H
