#ifndef NIMBLEEDITOR_DETAILSPANEL_H
#define NIMBLEEDITOR_DETAILSPANEL_H

#if WITH_EDITOR

#include <cstdint>
#include <map>
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
 * The selected entity: its members (name, script), then one collapsible
 * section per component with its registered properties (a header even if it
 * has none).
 *
 *   Entity         Name, Script, the script's run status, cull flag.
 *   Per property   a reset arrow while the value differs from the default.
 *   Per array      a list: the entries in use, each removable (later ones
 *                  move up), and "+ Add" for the next.
 *   Per component  Reset and Remove in the header.
 *   At the bottom  Add Component (addable types the entity lacks).
 *
 * Rebuilt when the selection or component set changes; values are re-read on
 * a short timer to pick up outside changes.
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

    /* An array property's list as built: rebuilt when it must show more. */
    struct ArrayList
    {
        GameCore::ComponentTypeID        Type  = GameCore::COMPONENT_TYPE_INVALID;
        GameCore::ComponentPropertyEntry Entry;
        size_t                           Shown = 0;
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

    /* Erases element InIndex: the later ones move up, the last is the default. */
    void RemoveArrayElement(GameCore::ComponentTypeID InType, const GameCore::ComponentPropertyEntry& InEntry, size_t InIndex);

    /* Rebuild once the current event is done (a button may be deleting itself). */
    void QueueRebuild();

    /* Sets InEntity's script to InFile (.lua or .script.nmgraph), as the game names it. */
    void UseScriptFile(GameCore::EntityID InEntity, const QString& InFile);

protected:
    /* The Script field's drops. */
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:

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
    std::vector<ArrayList>                 Arrays;

    /* How many entries each array shows for the shown entity, by
     * (type << 32 | offset): at least those in use, plus any added. */
    std::map<uint64_t, size_t> ArrayShown;
    bool                       bRebuildQueued = false;

    QTimer RefreshTimer;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_DETAILSPANEL_H
