#ifndef NIMBLEEDITOR_EDITORPLAYERSTARTS_H
#define NIMBLEEDITOR_EDITORPLAYERSTARTS_H

#if WITH_EDITOR

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

class EditorDocument;

/*
 * Making Player Starts (PlayerStart.h): World > Add Player Start and the
 * Outliner's context menu. A new entity named "Player Start" with a
 * Transform at the given spot, turned to InYawDegrees, and a default
 * PlayerStart component -- one undo step, then selected. The viewport draws
 * it as a capsule with an arrow the way the avatar will face.
 */
namespace EditorPlayerStarts
{
    GameCore::EntityID CreatePlayerStart(EditorDocument& InDocument, const EngineCore::Vector3& InPosition, double InYawDegrees = 0.0);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORPLAYERSTARTS_H
