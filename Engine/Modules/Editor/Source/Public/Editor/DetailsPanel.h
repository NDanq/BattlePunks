#ifndef NIMBLEEDITOR_DETAILSPANEL_H
#define NIMBLEEDITOR_DETAILSPANEL_H

#if WITH_EDITOR

#include <memory>
#include <vector>

#include <QTimer>
#include <QWidget>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

class EditorContext;
class PropertyEditor;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

/*
 * The selected entity: first its members (name and script path -- not
 * components, so not removable), then its components, one collapsible section
 * each, showing only the fields its type registered with
 * GAMECORE_DEFINE_COMPONENT_PROPERTIES. A component with no property list
 * still gets a header, so the panel tells the whole truth about what the
 * entity carries.
 *
 *   Entity         Name, Script (a .lua path, typed or browsed), the script's
 *                  run status (only a playing World runs scripts) and its
 *                  cull-outside-active-region flag.
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

    QWidget* BuildEntitySection(GameCore::EntityID InEntity);
    void     RefreshEntitySection();
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

    /* The Entity section's editors; null when nothing is shown. */
    QLineEdit* NameEdit     = nullptr;
    QLineEdit* ScriptEdit   = nullptr;
    QLabel*    ScriptStatus = nullptr;
    QCheckBox* CullCheck    = nullptr;

    GameCore::EntityID                     ShownEntity;
    std::vector<GameCore::ComponentTypeID> ShownTypes;
    std::vector<PropertyRow>               Rows;
    std::vector<ComponentSection>          Sections;

    QTimer RefreshTimer;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_DETAILSPANEL_H
