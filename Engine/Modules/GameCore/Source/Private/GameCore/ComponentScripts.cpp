#include "Public/GameCore/ComponentScript.h"

#include "Public/GameCore/Components.h"

/*
 * What the built-in components expose to Lua. Anything not listed here is
 * invisible to scripts -- above all the per-run state (Owner, ProxyID).
 * Every component also has GetEntity(), added by the engine.
 */

namespace GameCore
{
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

    /* GetEnabled/SetEnabled, GetLayer/SetLayer, GetMask/SetMask,
     * GetOverlapCount (read-only), GetSource/SetSource ("Bounds", "Mesh"),
     * IsOverlapping(other). Scripts react through OnOverlapBegin(self, other)
     * and OnOverlapEnd(self, other) on their class table. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(CollisionComponent)
    {
        Bind.Flag("Enabled", &CollisionComponent::bEnabled);
        Bind.Field("Layer", &CollisionComponent::Layer);
        Bind.Field("Mask",  &CollisionComponent::Mask);
        Bind.ReadOnly("OverlapCount", &CollisionComponent::OverlapCount);

        Bind.NativePure<&CollisionComponent::GetSourceName>("GetSource", "-> Source");
        Bind.Native<&CollisionComponent::SetSourceName>("SetSource", "Source");
        Bind.NativePure<&CollisionComponent::IsOverlapping>("IsOverlapping", "Other -> Overlapping");
    }

    /* GetLocal/SetLocal (six numbers), GetWorld (read-only -- the refit owns
     * it), GetStatic/SetStatic (boolean). ProxyID stays hidden. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(BoundsComponent)
    {
        Bind.Field("Local", &BoundsComponent::Local);
        Bind.ReadOnly("World", &BoundsComponent::World);
        Bind.Flag("Static", &BoundsComponent::bStatic);
    }
}
