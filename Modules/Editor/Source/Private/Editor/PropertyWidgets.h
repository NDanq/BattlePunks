#ifndef NIMBLEEDITOR_PROPERTYWIDGETS_H
#define NIMBLEEDITOR_PROPERTYWIDGETS_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>

#include <QWidget>

#include "Public/GameCore/ComponentProperties.h"

/*
 * One widget per ComponentPropertyEntry, by type. It knows only the field's
 * offset and type; a resolver returns the live component bytes on every
 * access (pool pointers are never held).
 *
 *   ReadFrom   live value -> widget, skipped while the widget has focus.
 *   Written    emitted after a user edit is stored.
 *
 * PF_ReadOnly entries never write.
 */
class PropertyEditor : public QWidget
{
    Q_OBJECT

public:
    using ResolveFn = std::function<uint8_t*()>;

    PropertyEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent);

    /* Skipped while focused unless bInForce (e.g. after a reset). */
    void Refresh(bool bInForce = false);

    const GameCore::ComponentPropertyEntry& GetEntry() const { return Entry; }

signals:
    void Written();

protected:
    /* InField points at this property's bytes inside the live component. */
    virtual void ReadFrom(const uint8_t* InField) = 0;

    /* Null if the component is gone or the property is read-only. */
    uint8_t* FieldForWrite() const;

    void NotifyWritten() { emit Written(); }

    /* True while any child has keyboard focus -- the user is mid-edit. */
    bool IsBeingEdited() const;

    GameCore::ComponentPropertyEntry Entry;
    ResolveFn                        Resolve;
};

/* Value comparison against the default: Vector3/AABB on X/Y/Z only (ignoring
 * the SIMD lane), FixedString up to NUL, else bytes. */
bool IsPropertyAtDefault(const GameCore::ComponentPropertyEntry& InEntry, const uint8_t* InField, const uint8_t* InDefault);

/* nullptr for a type with no editor (none today; kept for future types). */
PropertyEditor* CreatePropertyEditor(const GameCore::ComponentPropertyEntry& InEntry,
                                     PropertyEditor::ResolveFn InResolve, QWidget* InParent);

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROPERTYWIDGETS_H
