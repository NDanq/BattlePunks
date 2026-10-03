#ifndef NIMBLEEDITOR_EDITORPRIMITIVES_H
#define NIMBLEEDITOR_EDITORPRIMITIVES_H

#if WITH_EDITOR

#include <QMenu>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

class EditorDocument;

/*
 * Placing the engine's primitive meshes (Engine/Content/Meshes): World > Add
 * Primitive and the Outliner's context menu. Each is a new entity named for
 * it, with a Transform at the given spot and a Render component drawing the
 * mesh with DefaultLit -- one undo step, then selected. The meshes are one
 * unit across, so each is scaled to a size worth seeing: a 100-unit box or
 * sphere, a 1000 x 1000 plane facing up (+Y).
 */
namespace EditorPrimitives
{
    enum class EKind { Box, Sphere, Plane };

    GameCore::EntityID CreatePrimitive(EditorDocument& InDocument, EKind InKind, const EngineCore::Vector3& InPosition);

    /* "Box", "Sphere", "Plane" into InMenu; each creates at what InWhere
     * returns when chosen. */
    template<typename TWhere>
    void AddPrimitiveActions(QMenu* InMenu, EditorDocument& InDocument, TWhere InWhere)
    {
        const struct { const char* Text; EKind Kind; } Entries[] = {
            { "&Box", EKind::Box }, { "&Sphere", EKind::Sphere }, { "&Plane", EKind::Plane },
        };
        for(const auto& Entry : Entries)
        {
            const EKind Kind = Entry.Kind;
            InMenu->addAction(QString::fromLatin1(Entry.Text), InMenu, [&InDocument, InWhere, Kind]()
            {
                CreatePrimitive(InDocument, Kind, InWhere());
            });
        }
    }
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORPRIMITIVES_H
