#ifndef NIMBLEEDITOR_COMPONENTPICKER_H
#define NIMBLEEDITOR_COMPONENTPICKER_H

#if WITH_EDITOR

#include <vector>

#include <QFrame>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace GameCore { class World; }

/*
 * The Details panel's "Add Component" popup: a search box over every addable
 * component type, filed in folders by Props.TypeCategory ("Other" if unset).
 * Types the entity already has are listed greyed out. Read when built, so
 * open a new one per click. Deletes itself when closed.
 */
class ComponentPicker : public QFrame
{
    Q_OBJECT

public:
    ComponentPicker(GameCore::World& InWorld, GameCore::EntityID InEntity, QWidget* InParent);

    /* Below InAnchor (above, if that runs off the screen), at least as wide. */
    void PopupBelow(const QWidget* InAnchor);

signals:
    void Chosen(GameCore::ComponentTypeID InType);

protected:
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    struct Entry
    {
        GameCore::ComponentTypeID Type = GameCore::COMPONENT_TYPE_INVALID;
        QString                   Label;
        QString                   Category;
        QString                   SearchText;   // lower-case: label and category
        bool                      bPresent = false;
    };

    void Refill(const QString& InFilter);
    void ChooseCurrent();
    void Choose(QTreeWidgetItem* InItem);

    std::vector<Entry> Entries;
    QLineEdit*         Search = nullptr;
    QTreeWidget*       Tree   = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_COMPONENTPICKER_H
