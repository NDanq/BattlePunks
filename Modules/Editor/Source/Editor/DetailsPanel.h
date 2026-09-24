#ifndef NIMBLEEDITOR_DETAILSPANEL_H
#define NIMBLEEDITOR_DETAILSPANEL_H

#include <memory>
#include <vector>

#include <QTimer>
#include <QWidget>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Editor/PropertyWidgets.h"

class EditorContext;
class QLabel;
class QMenu;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

/*
 * The selected entity's components, one collapsible section each, showing
 * only the fields its type registered with GAMECORE_DEFINE_COMPONENT_PROPERTIES.
 * A component with no property list still gets a header, so the panel tells
 * the whole truth about what the entity carries.
 *
 *   Per property   a reset arrow to the right, shown only while the value
 *                  differs from the type's default-constructed one; clicking
 *                  it puts that property back.
 *   Per component  Reset (every writable property to default) and Remove, in
 *                  the section header.
 *   At the bottom  Add Component: every type with a property list that the
 *                  entity lacks and that did not opt out (CPF_NotAddable),
 *                  added from its default-constructed bytes.
 *
 * Rebuilt when the selection or the entity's set of components changes;
 * otherwise the editors and reset arrows re-read their values on a short
 * timer, so a change made elsewhere (the viewport, a script) shows up
 * without a signal per field.
 */
class DetailsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DetailsPanel(EditorContext& InContext, QWidget* InParent = nullptr);

private:
    /* One property's editor and its reset arrow. */
    struct PropertyRow
    {
        PropertyEditor*                   Editor = nullptr;
        QToolButton*                      Reset  = nullptr;   // null for a read-only property
        GameCore::ComponentTypeID         Type   = GameCore::COMPONENT_TYPE_INVALID;
        GameCore::ComponentPropertyEntry  Entry;
    };

    /* One component section: its default bytes and its header Reset. */
    struct ComponentSection
    {
        GameCore::ComponentTypeID                   Type = GameCore::COMPONENT_TYPE_INVALID;
        std::shared_ptr<const std::vector<uint8_t>> Defaults;   // empty if the type has none
        QToolButton*                                ResetAll = nullptr;
    };

    void Rebuild();
    void RebuildIfComponentsChanged();
    void RefreshValues();
    void UpdateHeader();

    /* The component types InEntity carries, in type-id order. */
    std::vector<GameCore::ComponentTypeID> CollectComponentTypes(GameCore::EntityID InEntity) const;

    QWidget* BuildComponentSection(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);
    QWidget* BuildAddComponentButton();
    void     PopulateAddMenu(QMenu* InMenu);

    /* The live bytes of InType on the shown entity, or null. */
    uint8_t* ResolveComponent(GameCore::ComponentTypeID InType) const;
    const ComponentSection* FindSection(GameCore::ComponentTypeID InType) const;

    /* Copies InRow's default into the live component and tells the context. */
    void ResetProperty(const PropertyRow& InRow);
    void ResetComponent(GameCore::ComponentTypeID InType);

    EditorContext& Context;

    QLabel*      Header      = nullptr;
    QScrollArea* Scroll      = nullptr;
    QWidget*     Body        = nullptr;
    QVBoxLayout* BodyLayout  = nullptr;

    GameCore::EntityID                     ShownEntity;
    std::vector<GameCore::ComponentTypeID> ShownTypes;
    std::vector<PropertyRow>               Rows;
    std::vector<ComponentSection>          Sections;

    QTimer RefreshTimer;
};

#endif //NIMBLEEDITOR_DETAILSPANEL_H
