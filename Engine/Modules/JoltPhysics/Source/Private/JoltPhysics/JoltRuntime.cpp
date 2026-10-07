#include "Private/JoltPhysics/JoltRuntime.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"

/* Jolt's worker threads; 0 picks the hardware's threads less one (the game
 * thread steps too), at most 4. */
NIMBLE_CONFIG_INT(GPhysicsWorkerThreads, "Physics", "WorkerThreads", 0, nullptr);

namespace JoltPhysics::Runtime
{
    namespace
    {
        int                                 RefCount = 0;
        JPH::JobSystemThreadPool*           Jobs     = nullptr;
        JPH::TempAllocatorImplWithMallocFallback* Temp = nullptr;

        void TraceToLog(const char* InFormat, ...)
        {
            char Buffer[1024];
            va_list Args;
            va_start(Args, InFormat);
            std::vsnprintf(Buffer, sizeof(Buffer), InFormat, Args);
            va_end(Args);
            CORE_LOG(std::string("[Physics] Jolt: ") + Buffer)
        }

#ifdef JPH_ENABLE_ASSERTS
        /* Logged, not a breakpoint: a Debug game keeps running. */
        bool AssertToLog(const char* InExpression, const char* InMessage, const char* InFile, const JPH::uint InLine)
        {
            CORE_LOG_ERROR(std::string("[Physics] Jolt assert: ") + InExpression
                           + (InMessage ? std::string(" (") + InMessage + ")" : std::string())
                           + " at " + InFile + ":" + std::to_string(InLine))
            return false;
        }
#endif

        int WorkerThreads()
        {
            const int Configured = GPhysicsWorkerThreads.GetInt();
            if(Configured > 0) { return Configured; }
            const int Hardware = int(std::thread::hardware_concurrency());
            return std::clamp(Hardware - 1, 1, 4);
        }
    }

    bool Acquire()
    {
        if(RefCount++ > 0)
        {
            return true;
        }

        JPH::RegisterDefaultAllocator();
        JPH::Trace = &TraceToLog;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &AssertToLog;)

        if(!JPH::VerifyJoltVersionID())
        {
            CORE_LOG_ERROR("[Physics] Jolt was built with different settings than JoltPhysics "
                           "(JPH_* defines or SIMD flags); physics is off")
            RefCount = 0;
            return false;
        }

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        const int Threads = WorkerThreads();
        Jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, Threads);
        Temp = new JPH::TempAllocatorImplWithMallocFallback(16 * 1024 * 1024);

        CORE_LOG("[Physics] Jolt " + std::to_string(JPH_VERSION_MAJOR) + "." + std::to_string(JPH_VERSION_MINOR)
                 + "." + std::to_string(JPH_VERSION_PATCH) + " started, " + std::to_string(Threads) + " worker thread(s)")
        return true;
    }

    void Release()
    {
        if(RefCount <= 0 || --RefCount > 0)
        {
            return;
        }

        delete Temp;
        Temp = nullptr;
        delete Jobs;
        Jobs = nullptr;

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    JPH::JobSystem& GetJobSystem()         { return *Jobs; }
    JPH::TempAllocator& GetTempAllocator() { return *Temp; }
}
