#include "Public/GameCore/Components.h"

/*
 * The engine-built-in components' registration, plus TransformComponent's
 * one out-of-line method. See DESIGN.md for why GetLocalMatrix() needs three
 * separate Matrix4x4::InitRotation calls rather than one, and for the
 * Z * Y * X composition order.
 */

namespace GameCore
{
    EngineCore::Matrix4x4 PoseToMatrix(const TransformPose& InPose)
    {
        EngineCore::Matrix4x4 T; T.InitTranslation(InPose.Position);
        EngineCore::Matrix4x4 S; S.InitScale(InPose.Scale);

        EngineCore::Matrix4x4 Rx; Rx.InitRotation(float(InPose.Rotation.GetX()), EngineCore::Vector3(1.0, 0.0, 0.0));
        EngineCore::Matrix4x4 Ry; Ry.InitRotation(float(InPose.Rotation.GetY()), EngineCore::Vector3(0.0, 1.0, 0.0));
        EngineCore::Matrix4x4 Rz; Rz.InitRotation(float(InPose.Rotation.GetZ()), EngineCore::Vector3(0.0, 0.0, 1.0));

        return T * (Rz * Ry * Rx) * S;
    }

    EngineCore::Matrix4x4 TransformComponent::GetLocalMatrix() const
    {
        return PoseToMatrix(GetPose());
    }

    GAMECORE_DEFINE_COMPONENT(TransformComponent, "Transform")
    GAMECORE_DEFINE_COMPONENT(BoundsComponent,    "Bounds")
    GAMECORE_DEFINE_COMPONENT(CollisionComponent, "Collision")
}
