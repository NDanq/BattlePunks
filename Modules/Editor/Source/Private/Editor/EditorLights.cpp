#if WITH_EDITOR

#include "Private/Editor/EditorLights.h"

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/Editor/EditorDocument.h"

namespace EditorLights
{
    GameCore::EntityID CreateLight(EditorDocument& InDocument, const EKind InKind, const EngineCore::Vector3& InPosition)
    {
        QString             Name;
        EngineCore::Vector3 Rotation;
        switch(InKind)
        {
            case EKind::Point:       Name = QStringLiteral("Point Light"); break;
            case EKind::Spot:        Name = QStringLiteral("Spot Light");        Rotation = EngineCore::Vector3(45.0, 0.0, 0.0);  break;
            case EKind::Directional: Name = QStringLiteral("Directional Light"); Rotation = EngineCore::Vector3(50.0, 30.0, 0.0); break;
            case EKind::Sky:         Name = QStringLiteral("Sky Light"); break;
            case EKind::CaptureOmni:   Name = QStringLiteral("Scene Capture Probe"); break;
            case EKind::CaptureMirror: Name = QStringLiteral("Mirror Probe"); break;
        }

        const GameCore::EntityID Light = InDocument.CreateEntity(Name, InPosition, [InKind, Rotation](const GameCore::Entity& InEntity)
        {
            if(GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>())
            {
                Transform->Rotation = Rotation;
            }
            switch(InKind)
            {
                case EKind::Point:       InEntity.AddComponent<RenderCore::PointLightComponent>();       break;
                case EKind::Spot:        InEntity.AddComponent<RenderCore::SpotLightComponent>();        break;
                case EKind::Directional: InEntity.AddComponent<RenderCore::DirectionalLightComponent>(); break;
                case EKind::Sky:         InEntity.AddComponent<RenderCore::SkyLightComponent>();         break;
                case EKind::CaptureOmni:
                {
                    /* Six views a capture: every half second, not every frame, to start with. */
                    RenderCore::SceneCaptureProbeComponent Probe;
                    Probe.Mode      = RenderCore::SCM_Omnidirectional;
                    Probe.Frequency = RenderCore::SCF_Interval;
                    Probe.Interval  = 0.5f;
                    InEntity.AddComponent(Probe);
                    break;
                }
                case EKind::CaptureMirror:
                {
                    RenderCore::SceneCaptureProbeComponent Probe;
                    Probe.Mode = RenderCore::SCM_PlanarMirror;
                    InEntity.AddComponent(Probe);
                    break;
                }
            }
        });
        InDocument.SetSelection(Light);
        return Light;
    }
}

#endif // WITH_EDITOR
