#ifndef NIMBLEEDITOR_DETAILSPANEL_H
#define NIMBLEEDITOR_DETAILSPANEL_H

#include <vector>

#include <QTimer>
#include <QWidget>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

class EditorContext;
class PropertyEditor;
class QLabel;
class QScrollArea;
class QVBoxLayout;

/*
 * The selected entity's components, one collapsible section each, showing
 * only the fields its type registered with GAMECORE_DEFINE_COMPONENT_PROPERTIES.
 * A component with no property list still gets a header, so the panel tells
 * the whole truth about what the entity carries.
 *
 * Rebuilt when the selection or the entity's set of components changes;
 * otherwise the editors re-read their values on a short timer, so a change
 * made elsewhere (the viewport, a future undo) shows up without a signal per
 * field.
 */
class DetailsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DetailsPanel(EditorContext& InContext, QWidget* InParent = nullptr);

private:
    void Rebuild();
    void RebuildIfComponentsChanged();
    void RefreshValues();

    /* The component types InEntity carries, in type-id order. */
    std::vector<GameCore::ComponentTypeID> CollectComponentTypes(GameCore::EntityID InEntity) const;

    QWidget* BuildComponentSection(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    EditorContext& Context;

    QLabel*      Header      = nullptr;
    QScrollArea* Scroll      = nullptr;
    QWidget*     Body        = nullptr;
    QVBoxLayout* BodyLayout  = nullptr;

    GameCore::EntityID                     ShownEntity;
    std::vector<GameCore::ComponentTypeID> ShownTypes;
    std::vector<PropertyEditor*>           Editors;

    QTimer RefreshTimer;
};

#endif //NIMBLEEDITOR_DETAILSPANEL_H
