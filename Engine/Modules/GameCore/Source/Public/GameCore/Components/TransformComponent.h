#ifndef GAMECORE_TRANSFORMCOMPONENT_H
#define GAMECORE_TRANSFORMCOMPONENT_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

namespace GameCore
{
    /* "Transform". Position, rotation (Euler degrees) and scale. 64 bytes
     * total (one cache line) -- see DESIGN.md. */
    struct GAMECORE_API TransformComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;   // Euler degrees, applied Z * Y * X -- see DESIGN.md
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};

        /* Translation * (Rz * Ry * Rx) * Scale, relative to the parent if
         * the entity has one -- Entity::GetWorldMatrix() is where it is. */
        EngineCore::Matrix4x4 GetLocalMatrix() const;

        TransformPose GetPose() const
        {
            TransformPose Pose;
            Pose.Position = Position;
            Pose.Rotation = Rotation;
            Pose.Scale    = Scale;
            return Pose;
        }
        void SetPose(const TransformPose& InPose)
        {
            Position = InPose.Position;
            Rotation = InPose.Rotation;
            Scale    = InPose.Scale;
        }

        /* World space, through the entity's parents -- Entity::GetWorldPose.
         * The setters write the local values that land there. */
        EngineCore::Vector3 GetWorldPosition() const { return Owner.GetEntity().GetWorldPose().Position; }
        EngineCore::Vector3 GetWorldRotation() const { return Owner.GetEntity().GetWorldPose().Rotation; }
        EngineCore::Vector3 GetWorldScale() const    { return Owner.GetEntity().GetWorldPose().Scale; }
        void SetWorldPosition(const EngineCore::Vector3& InPosition)
        {
            TransformPose Pose = Owner.GetEntity().GetWorldPose();
            Pose.Position = InPosition;
            Owner.GetEntity().SetWorldPose(Pose);
        }
        void SetWorldRotation(const EngineCore::Vector3& InRotation)
        {
            TransformPose Pose = Owner.GetEntity().GetWorldPose();
            Pose.Rotation = InRotation;
            Owner.GetEntity().SetWorldPose(Pose);
        }
    };

    /* TransformComponent::GetLocalMatrix's composition, for any pose. */
    GAMECORE_API EngineCore::Matrix4x4 PoseToMatrix(const TransformPose& InPose);
}

#endif //GAMECORE_TRANSFORMCOMPONENT_H
