#ifndef NIMBLEEDITOR_UIPALETTEPANEL_H
#define NIMBLEEDITOR_UIPALETTEPANEL_H

#if WITH_EDITOR

#include <QTreeWidget>

/*
 * The widget types a document can hold, by palette group (UI::UIWidgetType::
 * Palette), from the registry -- so a game module's own widgets appear while
 * it is loaded. Drag one onto the canvas or the hierarchy, or double-click it
 * to add it to the selection.
 */
class UIPalettePanel : public QTreeWidget
{
    Q_OBJECT

public:
    /* A drag's payload: the type's XML name. */
    static constexpr const char* MIME_TYPE = "application/x-nimble-ui-widget";

    explicit UIPalettePanel(QWidget* InParent = nullptr);

    /* Lists the registry again (a game module loaded or unloaded). */
    void Refresh();

signals:
    void AddRequested(const QString& InType);

protected:
    QMimeData*  mimeData(const QList<QTreeWidgetItem*>& InItems) const override;
    QStringList mimeTypes() const override;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIPALETTEPANEL_H
