#if WITH_EDITOR

#include "Private/Editor/EditorPrimitives.h"

#include <cstdio>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/Editor/EditorDocument.h"

namespace EditorPrimitives
{
    GameCore::EntityID CreatePrimitive(EditorDocument& InDocument, const EKind InKind, const EngineCore::Vector3& InPosition)
    {
        QString             Name;
        const char*         Mesh = "";
        EngineCore::Vector3 Scale(100.0, 100.0, 100.0);
        switch(InKind)
        {
            case EKind::Box:    Name = QStringLiteral("Box");    Mesh = "Engine/Content/Meshes/Box.nmmesh";    break;
            case EKind::Sphere: Name = QStringLiteral("Sphere"); Mesh = "Engine/Content/Meshes/Sphere.nmmesh"; break;
            case EKind::Plane:  Name = QStringLiteral("Plane");  Mesh = "Engine/Content/Meshes/Plane.nmmesh";
                                Scale = EngineCore::Vector3(1000.0, 1.0, 1000.0); break;
        }

        const GameCore::EntityID Primitive = InDocument.CreateEntity(Name, InPosition, [Mesh, Scale](const GameCore::Entity& InEntity)
        {
            if(GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>())
            {
                Transform->Scale = Scale;
            }
            RenderCore::RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", Mesh);
            InEntity.AddComponent(Render);
        });
        InDocument.SetSelection(Primitive);
        return Primitive;
    }
}

#endif // WITH_EDITOR
