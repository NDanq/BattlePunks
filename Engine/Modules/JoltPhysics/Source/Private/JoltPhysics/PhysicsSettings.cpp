#include "Public/JoltPhysics/PhysicsWorld.h"

#include <algorithm>

#include "Public/Core/CoreConfig.h"

/* [Physics]: how each level's World is simulated (README.md). Read when a
 * level loads, so a change takes effect at the next one. */
NIMBLE_CONFIG_FLOAT(GPhysicsUnitsPerMeter, "Physics", "UnitsPerMeter", 100.0f, nullptr);   // world units in a metre
NIMBLE_CONFIG_FLOAT(GPhysicsGravityY, "Physics", "GravityY", -981.0f, nullptr);             // world units / s^2, along Y
NIMBLE_CONFIG_FLOAT(GPhysicsFixedHz, "Physics", "FixedHz", 60.0f, nullptr);                 // fixed steps a second
NIMBLE_CONFIG_INT(GPhysicsMaxSubSteps, "Physics", "MaxSubSteps", 4, nullptr);               // most steps in one frame
NIMBLE_CONFIG_BOOL(GPhysicsInterpolate, "Physics", "Interpolate", true, nullptr);           // blend Transforms between steps
NIMBLE_CONFIG_INT(GPhysicsMaxBodies, "Physics", "MaxBodies", 16384, nullptr);

namespace JoltPhysics
{
    PhysicsSettings PhysicsSettings::FromConfig()
    {
        PhysicsSettings Settings;
        Settings.UnitsPerMeter = GPhysicsUnitsPerMeter.GetFloat() > 0.0f ? double(GPhysicsUnitsPerMeter.GetFloat()) : 100.0;
        Settings.Gravity       = EngineCore::Vector3(0.0, double(GPhysicsGravityY.GetFloat()), 0.0);
        Settings.FixedHz       = std::clamp(GPhysicsFixedHz.GetFloat(), 10.0f, 1000.0f);
        Settings.MaxSubSteps   = uint32_t(std::clamp(GPhysicsMaxSubSteps.GetInt(), 1, 16));
        Settings.bInterpolate  = GPhysicsInterpolate.GetBool();
        Settings.MaxBodies     = uint32_t(std::clamp(GPhysicsMaxBodies.GetInt(), 64, 1 << 20));
        return Settings;
    }
}
