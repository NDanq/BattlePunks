#include "Public/GameCore/Components.h"

/*
 * The four engine-built-in components' registration, plus TransformComponent's
 * one out-of-line method.
 *
 * TransformComponent::GetLocalMatrix() composes Translation * (Rz * Ry * Rx) *
 * Scale from three SEPARATE Matrix4x4::InitRotation calls -- InitRotation(angle,
 * axis) sets a matrix to ONE axis-angle rotation and returns *this by value, it
 * does not compose an existing matrix with a new rotation, so three Euler
 * angles need three matrices multiplied together, not three calls into the
 * same one. Z * Y * X (rotate about X first, then Y, then Z) matches the
 * common "yaw-pitch-roll composed outermost-last" convention and is the
 * arbitrary-but-stated choice this component makes; a game that wants a
 * different composition order rolls its own component rather than this one
 * silently meaning something else than what its member name suggests.
 */

namespace GameCore
{
    EngineCore::Matrix4x4 TransformComponent::GetLocalMatrix() const
    {
        EngineCore::Matrix4x4 T; T.InitTranslation(Position);
        EngineCore::Matrix4x4 S; S.InitScale(Scale);

        EngineCore::Matrix4x4 Rx; Rx.InitRotation(float(Rotation.GetX()), EngineCore::Vector3(1.0, 0.0, 0.0));
        EngineCore::Matrix4x4 Ry; Ry.InitRotation(float(Rotation.GetY()), EngineCore::Vector3(0.0, 1.0, 0.0));
        EngineCore::Matrix4x4 Rz; Rz.InitRotation(float(Rotation.GetZ()), EngineCore::Vector3(0.0, 0.0, 1.0));

        return T * (Rz * Ry * Rx) * S;
    }

    GAMECORE_DEFINE_COMPONENT(TransformComponent, "Transform")
    GAMECORE_DEFINE_COMPONENT(NameComponent,      "Name")
    GAMECORE_DEFINE_COMPONENT(BoundsComponent,    "Bounds")
    GAMECORE_DEFINE_COMPONENT(ScriptComponent,    "Script")
}
