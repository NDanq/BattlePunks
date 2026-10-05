#ifndef NIMBLEEDITOR_UIDETAILSPANEL_H
#define NIMBLEEDITOR_UIDETAILSPANEL_H

#if WITH_EDITOR

#include <functional>
#include <map>
#include <string>

#include <QScrollArea>

#include "Public/UI/UIProperty.h"

class QFormLayout;
class QVBoxLayout;
class UIDesignDocument;

/*
 * The selected widget's properties, built from its type's registry entry
 * (UI::UIWidgetType) -- a game's own widget gets a panel too -- grouped by
 * category. Each shows where its value comes from: set on the widget (bold,
 * with a reset button), from a style (italic), or its type's default. Slot
 * properties show as its parent reads them: a Canvas's, or a box's.
 *
 * A Bindable property takes {Bind:Key} text as well as a value. An Action
 * property offers the registered native actions. A bad value is refused (the
 * field turns red) and not written. With nothing selected it shows the
 * document's own settings and its styles.
 */
class UIDetailsPanel : public QScrollArea
{
    Q_OBJECT

public:
    UIDetailsPanel(UIDesignDocument& InDesign, QWidget* InParent = nullptr);

    /* The field editing InProperty of the selection, for tests; null if none. */
    QWidget* FindEditor(const QString& InProperty) const;

signals:
    void StatusText(const QString& InText);

private:
    /* Rebuilt on the next turn of the event loop: never under the editor
     * whose signal changed the document. */
    void ScheduleRebuild();
    void Rebuild();
    void BuildWidget(int InWidget);
    void BuildDocument();

    /* One row: InDesc's editor showing InValue, writing through InWrite. */
    QWidget* MakeEditor(const UI::UIPropertyDesc& InDesc, const QString& InValue, const std::function<bool(const QString&)>& InWrite);

    /* A type's default values, as text, by property name (made once). */
    const std::map<std::string, std::string>& GetDefaults(const UI::UIWidgetType& InType);

    UIDesignDocument& Design;
    QWidget*          Content = nullptr;
    QVBoxLayout*      Layout  = nullptr;
    bool              bRebuildPending = false;
    std::map<QString, QWidget*> Editors;
    std::map<std::string, std::map<std::string, std::string>> Defaults;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIDETAILSPANEL_H
