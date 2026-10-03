#ifndef RENDERCORE_LIGHTSHADOW_H
#define RENDERCORE_LIGHTSHADOW_H

#include <cstdint>

#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Vector.h"

/*
 * What every light component shares: its shadow technique, and the one
 * definition of where a light points.
 *
 * Each light carries the same flat shadow fields (bCastShadows,
 * ShadowTechnique, ShadowTint, ShadowBias, ShadowNormalBias) rather than a
 * nested struct, so the serializer, Lua and the Details panel see plain
 * members. RENDERPIPELINE.md's "Shadows" describes what each technique draws.
 */
namespace RenderCore
{
    /* ShadowTechnique's values. Cascaded is the directional light's only. */
    enum ELightShadowTechnique : uint8_t
    {
        LST_Standard  = 0,   // the shadow takes away this light's own contribution
        LST_Modulated = 1,   // this light is unshadowed; the shadow tints the lit result by ShadowTint
        LST_Cascaded  = 2,   // Standard, split into cascades along the view (directional only)
    };

    /* The way a light at InRotation (Euler degrees, as TransformComponent)
     * points: unit +Z through Rz * Ry * Rx. +Y is up, and pitch (X) +90 points
     * down (-Y): a positive pitch tips the nose down. */
    inline EngineCore::Vector3 LightForward(const EngineCore::Vector3& InRotation)
    {
        return GameCore::TransformMath::EulerToRotation(InRotation).GetAxis(2);
    }
}

#endif //RENDERCORE_LIGHTSHADOW_H
