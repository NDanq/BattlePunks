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
     * three numbers each; plus Translate(dx, dy, dz). */
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
