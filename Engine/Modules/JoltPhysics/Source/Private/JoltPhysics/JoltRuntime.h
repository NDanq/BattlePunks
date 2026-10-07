#ifndef JOLTPHYSICS_JOLTRUNTIME_H
#define JOLTPHYSICS_JOLTRUNTIME_H

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>

/*
 * Jolt's process-wide state, shared by every PhysicsWorld: the allocator
 * hooks, the factory and type registry, the trace and assert callbacks (to
 * CoreLog), one job system and one temp allocator. Reference counted: the
 * first PhysicsWorld to attach starts it, the last to detach stops it, so an
 * image that never simulates never spawns Jolt's threads.
 *
 * Game thread only. Every PhysicsWorld steps on it, one after another, which
 * is what makes sharing the temp allocator safe.
 */
namespace JoltPhysics::Runtime
{
    /* False, logged, if Jolt refused to start (its version check failed). */
    bool Acquire();
    void Release();

    JPH::JobSystem&     GetJobSystem();
    JPH::TempAllocator& GetTempAllocator();
}

#endif //JOLTPHYSICS_JOLTRUNTIME_H
