#ifndef NIMBLEEDITOR_PROPERTYWIDGETS_H
#define NIMBLEEDITOR_PROPERTYWIDGETS_H

#include <cstdint>
#include <functional>

#include <QWidget>

#include "Public/GameCore/ComponentProperties.h"

/*
 * One editor widget per ComponentPropertyEntry, chosen by its type. A widget
 * knows the field's offset and type and nothing else about the component --
 * the Details panel hands it a resolver that returns the component's live
 * bytes, fetched fresh on every read and write (a pool pointer is invalidated
 * by any add or remove on that pool, so none is ever held).
 *
 *   ReadFrom   copies the live value into the widget, unless the user is
 *              editing it right now (the widget has focus), so a periodic
 *              refresh never fights the keyboard.
 *   Written    emitted after a user edit has been stored into the component.
 *
 * Read-only entries (PF_ReadOnly) show their value and never write.
 */
class PropertyEditor : public QWidget
{
    Q_OBJECT

public:
    using ResolveFn = std::function<uint8_t*()>;

    PropertyEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent);

    /* Pulls the current value from the component, if it still exists. */
    void Refresh();

    const GameCore::ComponentPropertyEntry& GetEntry() const { return Entry; }

signals:
    void Written();

protected:
    /* InField points at this property's bytes inside the live component. */
    virtual void ReadFrom(const uint8_t* InField) = 0;

    /* The field's live bytes for a write, or null if the component is gone
     * or the property is read-only. */
    uint8_t* FieldForWrite() const;

    void NotifyWritten() { emit Written(); }

    /* True while any child has keyboard focus -- the user is mid-edit. */
    bool IsBeingEdited() const;

    GameCore::ComponentPropertyEntry Entry;
    ResolveFn                        Resolve;
};

/* nullptr for a type with no editor (none today; kept for future types). */
PropertyEditor* CreatePropertyEditor(const GameCore::ComponentPropertyEntry& InEntry,
                                     PropertyEditor::ResolveFn InResolve, QWidget* InParent);

#endif //NIMBLEEDITOR_PROPERTYWIDGETS_H
