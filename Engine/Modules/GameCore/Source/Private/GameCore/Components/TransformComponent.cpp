#include "Public/GameCore/Components/TransformComponent.h"

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/*
 * TransformComponent: registration, its one out-of-line method, and what it
 * exposes to saves, Lua and the editor. See DESIGN.md for why GetLocalMatrix()
 * needs three separate Matrix4x4::InitRotation calls rather than one, and for
 * the Z * Y * X composition order.
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

    /* Every field is real data. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(TransformComponent, 1)
    {
        Ar.Field("Position", C.Position);
        Ar.Field("Rotation", C.Rotation);
        Ar.Field("Scale",    C.Scale);
    }

    /* A networked entity's place (NetComponent): the server sends each as it
     * changes. Local, like the component itself. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(TransformComponent)
    {
        Rep.Field("Position", &TransformComponent::Position);
        Rep.Field("Rotation", &TransformComponent::Rotation);
        Rep.Field("Scale",    &TransformComponent::Scale);
    }

    /* GetPosition/SetPosition, GetRotation/SetRotation, GetScale/SetScale, as
     * three numbers each and local to the parent; plus Translate(dx, dy, dz)
     * and the world-space Get/SetWorldPosition, Get/SetWorldRotation and
     * GetWorldScale. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(TransformComponent)
    {
        Bind.Field("Position", &TransformComponent::Position);
        Bind.Field("Rotation", &TransformComponent::Rotation);
        Bind.Field("Scale",    &TransformComponent::Scale);

        Bind.Method("Translate", [](TransformComponent& C, ScriptCall& Call)
        {
            const EngineCore::Vector3 Delta = Call.GetVector3(1);
            if(Call.HasError()) { return; }   // do not apply a half-read delta
            C.Position = C.Position + Delta;
        }, "Delta:Vector3");

        Bind.NativePure<&TransformComponent::GetWorldPosition>("GetWorldPosition", "-> Position");
        Bind.Native<&TransformComponent::SetWorldPosition>("SetWorldPosition", "Position");
        Bind.NativePure<&TransformComponent::GetWorldRotation>("GetWorldRotation", "-> Rotation");
        Bind.Native<&TransformComponent::SetWorldRotation>("SetWorldRotation", "Rotation");
        Bind.NativePure<&TransformComponent::GetWorldScale>("GetWorldScale", "-> Scale");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(TransformComponent)
    {
        Props.Field("Position", &TransformComponent::Position);
        Props.Field("Rotation", &TransformComponent::Rotation);
        Props.Field("Scale",    &TransformComponent::Scale);
    }
#endif // WITH_EDITOR
}
