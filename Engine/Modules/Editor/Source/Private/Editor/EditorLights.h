#ifndef NIMBLEEDITOR_EDITORLIGHTS_H
#define NIMBLEEDITOR_EDITORLIGHTS_H

#if WITH_EDITOR

#include <QMenu>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

class EditorDocument;

/*
 * Making lights: World > Add Light and the Outliner's context menu. Each
 * light is a new entity named for it, with a Transform at the given spot and
 * the light component -- one undo step, then selected. A spot light and the
 * sun come aimed down at the ground (-Y; a positive pitch tips down).
 */
namespace EditorLights
{
    /* The lights, and the scene capture probes that light reflections. */
    enum class EKind { Point, Spot, Directional, Sky, CaptureOmni, CaptureMirror };

    GameCore::EntityID CreateLight(EditorDocument& InDocument, EKind InKind, const EngineCore::Vector3& InPosition);

    /* "Point Light", "Spot Light", "Directional Light", "Sky Light" into
     * InMenu; each creates at what InWhere returns when chosen. */
    template<typename TWhere>
    void AddLightActions(QMenu* InMenu, EditorDocument& InDocument, TWhere InWhere)
    {
        const struct { const char* Text; EKind Kind; } Entries[] = {
            { "&Point Light", EKind::Point }, { "&Spot Light", EKind::Spot },
            { "&Directional Light", EKind::Directional }, { "S&ky Light", EKind::Sky },
            { "Scene Capture Probe (&Omnidirectional)", EKind::CaptureOmni },
            { "Scene Capture Probe (Planar &Mirror)", EKind::CaptureMirror },
        };
        for(const auto& Entry : Entries)
        {
            if(Entry.Kind == EKind::CaptureOmni) { InMenu->addSeparator(); }
            const EKind Kind = Entry.Kind;
            InMenu->addAction(QString::fromLatin1(Entry.Text), InMenu, [&InDocument, InWhere, Kind]()
            {
                CreateLight(InDocument, Kind, InWhere());
            });
        }
    }
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORLIGHTS_H
