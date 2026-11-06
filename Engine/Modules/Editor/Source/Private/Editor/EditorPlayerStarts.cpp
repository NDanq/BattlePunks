#if WITH_EDITOR

#include "Private/Editor/EditorPlayerStarts.h"

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/Editor/EditorDocument.h"

namespace EditorPlayerStarts
{
    GameCore::EntityID CreatePlayerStart(EditorDocument& InDocument, const EngineCore::Vector3& InPosition, const double InYawDegrees)
    {
        const GameCore::EntityID Start = InDocument.CreateEntity(QStringLiteral("Player Start"), InPosition,
                                                                 [InYawDegrees](const GameCore::Entity& InEntity)
        {
            if(GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>())
            {
                Transform->Rotation = EngineCore::Vector3(0.0, InYawDegrees, 0.0);
            }
            InEntity.AddComponent<NimbleEngine::PlayerStartComponent>();
        });
        InDocument.SetSelection(Start);
        return Start;
    }
}

#endif // WITH_EDITOR
